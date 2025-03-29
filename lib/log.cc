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
    return seq > checkpoint_.stableSeq && seq < nextSeq_ && !log_.empty();
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    std::string prevDigest;

    // TODO check duplicates
    if (!clientRecord.update(c_id, c_seq)) {
        VLOG(4) << "Duplicate request detected by the log! c_id=" << c_id << " c_seq=" << c_seq;
        return false;
    }

    if (nextSeq_ - 1 == checkpoint_.stableSeq) {
        prevDigest = checkpoint_.stableLogDigest;
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
void Log::setCheckpoint(const LogCheckpoint &checkpoint)
{
    checkpoint_ = checkpoint;

    // Truncate log up to the checkpoint stable sequence
    log_.erase(log_.begin(), log_.begin() + (checkpoint.stableSeq - log_[0].seq + 1));

    // Commit app up to commitedSeq (potentially triggering a snapshot)
    app_->commit(checkpoint.committedSeq);
}

// Given a snapshot of the app state and corresponding checkpoint, reset log entirely to that state
bool Log::resetToSnapshot(const dombft::proto::SnapshotReply &snapshotReply)
{
    LogCheckpoint checkpoint(snapshotReply.checkpoint());

    // Try applying app snapshot if needed
    if (snapshotReply.has_app_snapshot()) {
        if (!app_->applySnapshot(snapshotReply.app_snapshot(), checkpoint.stableAppDigest, checkpoint.stableSeq)) {
            return false;
        }
    }

    checkpoint_ = checkpoint;
    clientRecord = checkpoint.clientRecord_;
    nextSeq_ = checkpoint.stableSeq + 1;
    latestCertSeq_ = 0;
    latestCert_.reset();
    log_.clear();

    // Apply LogEntries in snapshotReply
    std::string res;

    for (const dombft::proto::LogEntry &entry : snapshotReply.log_entries()) {
        if (!addEntry(entry.client_id(), entry.client_seq(), entry.request(), res)) {
            return false;
        }
    }

    if (getDigest() != checkpoint.committed_log_digest()) {
        // TODO, handle this case properly by resetting log state and requesting from another replica
        throw std::runtime_error("Snapshot digest mismatch!");
        return false;
    }

    return true;
}

// Given a snapshot of the state we want to try and match, change our checkpoint to match and reapply our logs
bool Log::applySnapshotModifyLog(const dombft::proto::SnapshotReply &snapshotReply)
{
    LogCheckpoint checkpoint(snapshotReply.checkpoint());

    // Number of missing entries in my log.
    uint32_t numMissing = clientRecord.numMissing(checkpoint.clientRecord_);

    // TODO this will fail if snapshotReply is invalid!
    // Remove all log entries before the checkpoint, but keep the last numMissing entries
    while (!log_.empty() && log_[0].seq <= checkpoint.committedSeq - numMissing) {
        log_.pop_front();
    }

    // Requests we want to try reapplying
    // Note this clears the current log
    std::deque<LogEntry> toReapply(log_);

    resetToSnapshot(snapshotReply);

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
        return checkpoint_.stableLogDigest;
    }
    return log_.back().digest;
}

const std::string &Log::getDigest(uint32_t seq) const
{
    if (seq == checkpoint_.stableSeq) {
        return checkpoint_.stableLogDigest;
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

LogCheckpoint &Log::getCheckpoint() { return checkpoint_; }

ClientRecord &Log::getClientRecord() { return clientRecord; }

void Log::toProto(dombft::proto::RepairStart &msg)
{
    dombft::proto::LogCheckpoint *checkpointProto = msg.mutable_checkpoint();

    checkpoint_.toProto(*checkpointProto);

    for (const LogEntry &entry : log_) {
        if (entry.seq <= checkpoint_.committedSeq) {
            continue;
        }

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
    out << "CHECKPOINT " << l.checkpoint_.committedSeq << ": " << digest_to_hex(l.checkpoint_.committedLogDigest)
        << " | ";
    for (const LogEntry &entry : l.log_) {
        out << entry;
    }
    return out;
}