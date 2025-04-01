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
    return seq > stableCheckpoint_.stableSeq && seq < nextSeq_ && !log_.empty();
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    std::string prevDigest;

    // TODO check duplicates
    if (!clientRecord_.update(c_id, c_seq)) {
        VLOG(4) << "Duplicate request detected by the log! c_id=" << c_id << " c_seq=" << c_seq;
        return false;
    }

    if (nextSeq_ - 1 == stableCheckpoint_.stableSeq) {
        prevDigest = stableCheckpoint_.stableLogDigest;
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
    const dombft::proto::Reply &r = cert.replies()[0];

    if (seq >= nextSeq_) {
        VLOG(3) << "Fail adding cert because seq=" << seq << " is greater than nextSeq=" << nextSeq_;
        return false;
    }

    if (seq <= committedCheckpoint_.committedSeq) {
        VLOG(5) << "Sequence for cert seq=" << seq << " has already been committed, checking client record";
        if (!committedCheckpoint_.clientRecord_.contains(r.client_id(), r.client_seq())) {
            VLOG(3) << "Fail adding cert because committed client record does not contain c_id=" << r.client_id()
                    << " c_seq=" << r.client_seq();
            return false;
        } else {
            // If this request has been committed in a checkpoint and has a valid cert, it must be committed!
            // We could send some COMMITTED_REPLY instead, but this works as well, and prevents the case where
            // some replicas might send a COMMITTED_REPLY and others a CERT_ACK
            return true;
        }
    }

    auto entry = getEntry(seq);   // will not be nullptr because range is checked above

    if (r.client_id() != entry.client_id || r.client_seq() != entry.client_seq || r.digest() != entry.digest) {
        VLOG(3) << "Fail adding cert because mismatching request!";
        return false;
    }

    VLOG(3) << "Added Cert for seq=" << seq << " c_id=" << r.client_id() << " c_seq=" << r.client_seq();
    // instead of adding the cert to the list of certs, directly update the latest cert.
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

    if (seq <= committedCheckpoint_.committedSeq) {
        LOG(ERROR) << "Tried to abort seq=" << seq
                   << " but seq is less than committedSeq=" << committedCheckpoint_.committedSeq;
        throw std::runtime_error("Tried to abort seq=" + std::to_string(seq) + " but seq is less than committedSeq.");
    }

    // If log is empty, we can't remove anything.
    if (!log_.empty()) {
        // remove all entries from seq to the end
        assert(log_[0].seq == stableCheckpoint_.stableSeq + 1);
        log_.erase(log_.begin() + (seq - log_[0].seq), log_.end());
    }

    app_->abort(seq);
    nextSeq_ = seq;
}

// Given a sequence number, commit the request and remove previous state, and save new checkpoint
void Log::setCheckpoint(const LogCheckpoint &newCheckpoint)
{
    if (newCheckpoint.committedSeq > committedCheckpoint_.committedSeq) {
        committedCheckpoint_ = newCheckpoint;

        VLOG(3) << "Log committing through seq " << newCheckpoint.committedSeq;

        // Commit app up to commitedSeq
        app_->commit(newCheckpoint.committedSeq);
    }

    if (newCheckpoint.stableSeq > stableCheckpoint_.stableSeq) {
        stableCheckpoint_ = newCheckpoint;

        // Truncate log up to the checkpoint stable sequence
        if (!log_.empty()) {
            int32_t stableIdx = newCheckpoint.stableSeq - log_[0].seq + 1;
            assert(stableIdx >= 0);
            log_.erase(log_.begin(), log_.begin() + stableIdx);
        }

        VLOG(3) << "Log truncating through seq " << newCheckpoint.stableSeq;
    }
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

        VLOG(2) << "Applying application snapshot for seq " << checkpoint.stableSeq;

        log_.clear();
        nextSeq_ = checkpoint.stableSeq + 1;

        // 1. When we reset to a app snapshot, our log is completely empty, so we need to set the checkpoint here
        // so it can be used as a previous digest.
        stableCheckpoint_ = checkpoint;
    }

    // TODO hack here to just clear client records and reset it afterwards
    clientRecord_ = ClientRecord();

    // Apply LogEntries in snapshotReply
    std::string res;
    for (const dombft::proto::LogEntry &entry : snapshotReply.log_entries()) {
        if (entry.seq() <= committedCheckpoint_.committedSeq) {
            // We may have a later checkpoint, can ignore extra messages
            continue;
        }

        if (!addEntry(entry.client_id(), entry.client_seq(), entry.request(), res)) {
            LOG(ERROR) << "Failed to add entry from snapshot " << entry.client_id() << " " << entry.client_seq();
            return false;
        }
    }

    // TODO end of above mentioned hack.
    clientRecord_ = checkpoint.clientRecord_;

    if (checkpoint.committedSeq > committedCheckpoint_.committedSeq) {
        committedCheckpoint_ = checkpoint;
    }

    VLOG(4) << "Checkpoint for seq " << checkpoint.committedSeq
            << " log digest after applying snapshot: " << digest_to_hex(getDigest())
            << " expected digest: " << digest_to_hex(checkpoint.committedLogDigest);

    if (getDigest() != checkpoint.committedLogDigest) {
        // TODO, handle this case properly by resetting log state and requesting from another replica
        throw std::runtime_error("Snapshot digest mismatch!");
        return false;
    }

    latestCertSeq_ = 0;
    latestCert_.reset();

    return true;
}

// Given a snapshot of the state we want to try and match, change our checkpoint to match and reapply our logs
bool Log::applySnapshotModifyLog(const dombft::proto::SnapshotReply &snapshotReply)
{
    LogCheckpoint checkpoint(snapshotReply.checkpoint());

    // Number of missing entries in my log.
    uint32_t numMissing = clientRecord_.numMissing(checkpoint.clientRecord_);

    // TODO this will fail if snapshotReply is invalid!
    // Requests we want to try reapplying
    std::deque<LogEntry> toReapply;

    for (uint32_t i = 0; i < log_.size(); i++) {
        if (log_[i].seq > checkpoint.committedSeq - numMissing) {
            toReapply.push_back(log_[i]);
        }
    }

    if (!resetToSnapshot(snapshotReply)) {
        return false;
    }

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
        return stableCheckpoint_.stableLogDigest;
    }
    return log_.back().digest;
}

const std::string &Log::getDigest(uint32_t seq) const
{
    if (seq == stableCheckpoint_.stableSeq) {
        return stableCheckpoint_.stableLogDigest;
    }

    if (!inRange(seq)) {
        LOG(ERROR) << "Tried to get digest of seq=" << seq << " but seq is out of range nextSeq=" << nextSeq_
                   << " stableSeq=" << committedCheckpoint_.stableSeq;
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

LogCheckpoint &Log::getCheckpoint() { return committedCheckpoint_; }

ClientRecord &Log::getClientRecord() { return clientRecord_; }

void Log::toProto(dombft::proto::RepairStart &msg)
{
    dombft::proto::LogCheckpoint *checkpointProto = msg.mutable_checkpoint();

    committedCheckpoint_.toProto(*checkpointProto);

    for (const LogEntry &entry : log_) {
        if (entry.seq <= committedCheckpoint_.committedSeq) {
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

    out << "STABLE_CHECKPOINT= " << l.stableCheckpoint_.stableSeq << " "
        << "COMMITCHECKPOINT=" << l.committedCheckpoint_.committedSeq << " | "
        << digest_to_hex(l.committedCheckpoint_.committedLogDigest) << " | ";
    for (const LogEntry &entry : l.log_) {
        if (entry.seq <= l.committedCheckpoint_.committedSeq) {
            continue;
        }

        out << entry;
    }
    return out;
}