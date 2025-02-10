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
    return seq > stableCheckpoint_.seq && seq < nextSeq_ && !log.empty();
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    std::string prevDigest;

    // TODO check duplicates
    if (clientRecord.update(c_id, c_seq)) {
        VLOG(4) << "Duplicate request detected by the log! c_id=" << c_id << " c_seq=" << c_seq;
        return false;
    }

    if (nextSeq_ - 1 == stableCheckpoint_.seq) {
        VLOG(4) << "Using checkpoint digest as previous for seq=" << nextSeq_;
        prevDigest = stableCheckpoint_.logDigest;
    } else {
        assert(!log.empty());
        prevDigest = log.back().digest;
    }

    log.emplace_back(nextSeq_, c_id, c_seq, req, prevDigest);

    res = app_->execute(req, nextSeq_);
    if (res.empty()) {
        LOG(WARNING) << "Application failed to execute request!";
    }
    log[nextSeq_ % log.size()].result = res;

    VLOG(4) << "Adding new entry at seq=" << nextSeq_ << " c_id=" << c_id << " c_seq=" << c_seq
            << " digest=" << digest_to_hex(log[nextSeq_ % log.size()].digest);

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

    if (r.client_id() != entry.client_id || r.client_seq() != entry.client_seq) {
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
    log.erase(log.begin() + (seq - log[0].seq), log.end());

    app_->abort(seq);   // TODO off by one?
    nextSeq_ = seq;
}

// Given a sequence number, commit the request and remove previous state, and save new checkpoint
void Log::setStableCheckpoint(const LogCheckpoint &checkpoint)
{
    stableCheckpoint_ = checkpoint;

    // Truncate log up to the checkpoint
    log.erase(log.begin(), log.begin() + (checkpoint.seq - log[0].seq + 1));
    app_->commit(checkpoint.seq);
}

// Given a snapshot of the app state and corresponding checkpoint, reset log entirely to that state
void Log::resetToSnapshot(uint32_t seq, const LogCheckpoint &checkpoint, const std::string &snapshot)
{
    stableCheckpoint_ = checkpoint;
    clientRecord = checkpoint.clientRecord_;
    nextSeq_ = seq + 1;
    latestCertSeq_ = 0;
    latestCert_ = std::nullopt;

    app_->applySnapshot(snapshot);
    log.clear();
}

// Given a snapshot of the state we want to try and match, change our checkpoint to match and reapply our logs
void Log::applySnapshotModifyLog(uint32_t seq, const LogCheckpoint &checkpoint, const std::string &snapshot)
{
    // Number of missing entries in my log.
    uint32_t numMissing = clientRecord.numMissing(checkpoint.clientRecord_);

    // Remove all log entries before the checkpoint, but keep the last numMissing entries
    while (!log.empty() && log[0].seq <= checkpoint.seq - numMissing) {
        log.pop_front();
    }

    // TODO check off by one here
    app_->applySnapshot(snapshot);
    clientRecord = checkpoint.clientRecord_;
    latestCert_.reset();
    latestCertSeq_ = 0;

    // Requests we want to try reapplying
    // Note this clears the current log
    std::deque<LogEntry> toReapply(std::move(log));
    nextSeq_ = checkpoint.seq + 1;

    // Reapply the rest of the entries in the log by modifying them in place
    for (LogEntry &entry : toReapply) {
        std::string temp;   // result gets stored here, but we don't need it
        addEntry(entry.client_id, entry.client_seq, entry.request, temp);
    }
}

uint32_t Log::getNextSeq() const { return nextSeq_; }

const std::string &Log::getDigest() const
{
    if (log.empty()) {
        return stableCheckpoint_.logDigest;
    }
    return log.back().digest;
}

const std::string &Log::getDigest(uint32_t seq) const
{
    if (!inRange(seq)) {
        throw std::runtime_error("Tried to get digest of seq=" + std::to_string(seq) + " but seq is out of range.");
    }

    uint32_t offset = log[0].seq;
    assert(log[seq - offset].seq == seq);
    return log[seq - offset].digest;
}

const LogEntry &Log::getEntry(uint32_t seq)
{
    if (!inRange(seq)) {
        throw std::runtime_error("Tried to get digest of seq=" + std::to_string(seq) + " but seq is out of range.");
    }
    uint32_t offset = log[0].seq;

    assert(log[seq - offset].seq == seq);
    return log[seq - offset];
}

LogCheckpoint &Log::getStableCheckpoint() { return stableCheckpoint_; }

ClientRecord &Log::getClientRecord() { return clientRecord; }

void Log::toProto(dombft::proto::FallbackStart &msg)
{
    dombft::proto::LogCheckpoint *checkpointProto = msg.mutable_checkpoint();

    stableCheckpoint_.toProto(*checkpointProto);

    for (const LogEntry &entry : log) {
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
    for (const LogEntry &entry : l.log) {
        out << entry;
    }
    return out;
}