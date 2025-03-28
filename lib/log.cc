#include "log.h"

#include "utils.h"

#include <glog/logging.h>

Log::Log(std::shared_ptr<Application> app)
    : nextSeq_(1)
    , app_(std::move(app))
{
}

bool Log::inRange(uint32_t seq) const
{
    // Note, log.empty() is implicitly checked by the first two conditions
    return seq > stableCheckpoint_.seq && seq < nextSeq_ && !log_.empty();
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    std::string prevDigest;

    // TODO check duplicates
    if (!clientRecord.update(c_id, c_seq)) {
        VLOG(4) << "Duplicate request detected by the log! c_id=" << c_id << " c_seq=" << c_seq;
        return false;
    }

    if (nextSeq_ - 1 == stableCheckpoint_.seq) {
        prevDigest = stableCheckpoint_.logDigest;
        VLOG(4) << "Using checkpoint digest as previous for seq=" << nextSeq_
                << " prevDigest=" << digest_to_hex(prevDigest);
    } else {
        assert(!log_.empty());
        prevDigest = log_.back().digest;
    }

    log_.emplace_back(nextSeq_, c_id, c_seq, req, prevDigest);

    dombft::proto::PaddedRequestData reqData;
    if (!reqData.ParseFromString(req)) {
        LOG(ERROR) << "Failed to parse request data!";
        return false;
    }

    res = app_->execute(reqData.req_data(), nextSeq_);
    if (res.empty()) {
        LOG(WARNING) << "Application failed to execute request!";
    }
    log_.back().result = res;

    VLOG(4) << "Adding new entry at seq=" << nextSeq_ << " c_id=" << c_id << " c_seq=" << c_seq
            << " digest=" << digest_to_hex(log_.back().digest);

    nextSeq_++;
    return true;
}

bool Log::addCert(uint32_t seq, const dombft::proto::Cert &cert)
{
    if (!inRange(seq)) {
        VLOG(5) << "Fail adding cert because out of range!";
        return false;
    }

    auto entry = getEntry(seq);   // will not be nullptr because range is checked above
    const dombft::proto::Reply &r = cert.replies()[0];

    if (r.client_id() != entry.client_id || r.client_seq() != entry.client_seq || r.digest() != entry.digest) {
        VLOG(5) << "Fail adding cert because mismatching request!";
        return false;
    }

    // instead of adding the cert to the list of certs, directly updating the latest cert.
    if (!latestCert_.has_value() || cert.seq() > latestCert_->seq()) {
        latestCert_ = cert;
        latestCertSeq_ = seq;
    }

    return true;
}

// Abort all requests starting from and including seq, as well as app state
void Log::abort(uint32_t seq)
{
    if (seq >= nextSeq_) {
        nextSeq_ = seq;
        return;
    }

    // remove all entries from seq to the end
    log_.erase(log_.begin() + (seq - log_[0].seq), log_.end());

    app_->abort(seq);
    nextSeq_ = seq;
}

// Given a sequence number, commit the request and remove previous state, and save new checkpoint
void Log::setStableCheckpoint(const LogCheckpoint &checkpoint)
{
    stableCheckpoint_ = checkpoint;

    // Truncate log up to the checkpoint
    log_.erase(log_.begin(), log_.begin() + (checkpoint.seq - log_[0].seq + 1));
    app_->commit(checkpoint.seq);
}

// Given a snapshot of the app state and corresponding checkpoint, reset log entirely to that state
bool Log::resetToSnapshot(uint32_t seq, const LogCheckpoint &checkpoint, const std::string &snapshot)
{
    stableCheckpoint_ = checkpoint;
    clientRecord = checkpoint.clientRecord_;
    nextSeq_ = seq + 1;
    latestCertSeq_ = 0;
    latestCert_.reset();
    log_.clear();

    return app_->applySnapshot(snapshot, checkpoint.appDigest, seq);
}

// Given a snapshot of the state we want to try and match, change our checkpoint to match and reapply our logs
bool Log::applySnapshotModifyLog(uint32_t seq, const LogCheckpoint &checkpoint, const std::string &snapshot)
{
    if (!app_->applySnapshot(snapshot, checkpoint.appDigest, seq)) {
        return false;
    }

    // Number of missing entries in my log.
    uint32_t numMissing = clientRecord.numMissing(checkpoint.clientRecord_);

    // Remove all log entries before the checkpoint, but keep the last numMissing entries
    while (!log_.empty() && log_[0].seq <= checkpoint.seq - numMissing) {
        log_.pop_front();
    }

    clientRecord = checkpoint.clientRecord_;
    latestCert_.reset();
    latestCertSeq_ = 0;
    stableCheckpoint_ = checkpoint;

    // Requests we want to try reapplying
    // Note this clears the current log
    std::deque<LogEntry> toReapply(log_);
    log_.clear();

    nextSeq_ = checkpoint.seq + 1;

    // Reapply the rest of the entries in the log
    for (LogEntry &entry : toReapply) {
        std::string temp;   // result gets stored here, but we don't need it
        addEntry(entry.client_id, entry.client_seq, entry.request, temp);
    }

    return true;
}

uint32_t Log::getNextSeq() const { return nextSeq_; }

const std::string &Log::getDigest() const
{
    if (log_.empty()) {
        return stableCheckpoint_.logDigest;
    }
    return log_.back().digest;
}

const std::string &Log::getDigest(uint32_t seq) const
{
    if (seq == stableCheckpoint_.seq) {
        return stableCheckpoint_.logDigest;
    }

    if (!inRange(seq)) {
        throw std::runtime_error("Tried to get digest of seq=" + std::to_string(seq) + " but seq is out of range.");
    }

    uint32_t offset = log_[0].seq;
    assert(log_[seq - offset].seq == seq);
    return log_[seq - offset].digest;
}

const LogEntry &Log::getEntry(uint32_t seq)
{
    if (!inRange(seq)) {
        throw std::runtime_error("Tried to get entry of seq=" + std::to_string(seq) + " but seq is out of range.");
    }
    uint32_t offset = log_[0].seq;

    assert(log_[seq - offset].seq == seq);
    return log_[seq - offset];
}

LogCheckpoint &Log::getStableCheckpoint() { return stableCheckpoint_; }

ClientRecord &Log::getClientRecord() { return clientRecord; }

void Log::toProto(dombft::proto::RepairStart &msg)
{
    dombft::proto::LogCheckpoint *checkpointProto = msg.mutable_checkpoint();

    stableCheckpoint_.toProto(*checkpointProto);

    for (const LogEntry &entry : log_) {
        dombft::proto::LogEntry *entryProto = msg.add_log_entries();
        entry.toProto(*entryProto);
    }

    if (latestCert_.has_value()) {
        *msg.mutable_cert() = latestCert_.value();
    }
}

std::ostream &operator<<(std::ostream &out, const Log &l)
{
    // go from nextSeq - MAX_SPEC_HIST, which traverses the whole buffer
    // starting from the oldest;
    out << "CHECKPOINT " << l.stableCheckpoint_.seq << ": " << digest_to_hex(l.stableCheckpoint_.logDigest) << " | ";
    for (const LogEntry &entry : l.log_) {
        out << entry;
    }
    return out;
}