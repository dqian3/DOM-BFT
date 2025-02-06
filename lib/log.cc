#include "log.h"

#include "utils.h"

#include <glog/logging.h>

Log::Log(std::shared_ptr<Application> app)
    : nextSeq(1)
    , lastExecuted(0)
    , app_(std::move(app))
{
}

bool Log::inRange(uint32_t seq) const
{
    // Note, log.empty() is implicitly checked by the first two conditions
    return seq > stableCheckpoint.seq && seq < nextSeq && !log.empty();
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    std::string prevDigest;
    if (nextSeq - 1 == stableCheckpoint.seq) {
        VLOG(4) << "Using checkpoint digest as previous for seq=" << nextSeq;
        prevDigest = stableCheckpoint.logDigest;
    } else {
        uint32_t offset = log[0].seq;
        prevDigest = log[nextSeq - offset].digest;
    }

    log.emplace_back(nextSeq, c_id, c_seq, req, prevDigest);

    res = app_->execute(req, nextSeq);
    if (res.empty()) {
        LOG(WARNING) << "Application failed to execute request!";
    }
    log[nextSeq % log.size()].result = res;

    VLOG(4) << "Adding new entry at seq=" << nextSeq << " c_id=" << c_id << " c_seq=" << c_seq
            << " digest=" << digest_to_hex(log[nextSeq % log.size()].digest).substr(56);

    nextSeq++;
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

const std::string &Log::getDigest() const
{
    if (log.empty()) {
        return;
    }
    return log.back().digest;
}

uint32_t Log::getNextSeq() const { return nextSeq; }

const std::string &Log::getDigest(uint32_t seq) const
{
    if (!inRange(seq)) {
        throw std::runtime_error("Tried to get digest of seq=" + std::to_string(seq) + " but seq is out of range.");
    }

    uint32_t offset = log[0].seq;
    return log[nextSeq - offset].digest;
}

void Log::toProto(dombft::proto::FallbackStart &msg)
{
    dombft::proto::LogCheckpoint *checkpointProto = msg.mutable_checkpoint();

    stableCheckpoint.toProto(*checkpointProto);

    for (const LogEntry &entry : log) {
        dombft::proto::LogEntry *entryProto = msg.add_log_entries();
        entry.toProto(*entryProto);
    }

    if (latestCert_.has_value()) {
        *msg.mutable_cert() = latestCert_.value();
    }
}

const LogEntry &Log::getEntry(uint32_t seq)
{
    if (inRange(seq)) {
    } else {
        throw std::runtime_error("Tried to get digest of seq=" + std::to_string(seq) + " but seq is out of range.");
    }

    uint32_t offset = log[0].seq;
    return log[seq - offset];
}

LogCheckpoint &Log::getStableCheckpoint() { return stableCheckpoint; }

void Log::commit(uint32_t seq)
{
    if (inRange(seq)) {
        app_.get()->commit(seq);
        certs.erase(certs.begin(), certs.lower_bound(seq));
    } else {
        LOG(ERROR) << "Sequence number " << seq << " is out of range.";
    }
}

std::ostream &operator<<(std::ostream &out, const Log &l)
{
    // go from nextSeq - MAX_SPEC_HIST, which traverses the whole buffer
    // starting from the oldest;
    out << "CHECKPOINT " << l.stableCheckpoint.seq << ": " << digest_to_hex(l.stableCheckpoint.logDigest).substr(56)
        << " | ";
    uint32_t i = l.stableCheckpoint.seq + 1;
    for (const LogEntry &entry : l.log) {
        out << entry;
    }
    return out;
}