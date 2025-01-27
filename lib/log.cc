#include "log.h"

#include "utils.h"

#include <glog/logging.h>

Log::Log(std::shared_ptr<Application> app)
    : nextSeq(1)
    , lastExecuted(0)
    , app_(std::move(app))
{
}

bool Log::inRange(uint32_t seq) const { return seq > checkpoint.seq && seq < nextSeq; }
bool Log::canAddEntry() const { return nextSeq <= checkpoint.seq + MAX_SPEC_HIST; }

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    std::string prevDigest;
    if (nextSeq - 1 == checkpoint.seq) {
        VLOG(4) << "Using checkpoint digest as previous for seq=" << nextSeq;
        prevDigest = checkpoint.logDigest;
    } else {
        uint32_t offset = log[0].seq;
        prevDigest = log[nextSeq - offset].digest;
    }

    if (!canAddEntry()) {
        throw std::runtime_error(
            "nextSeq = " + std::to_string(nextSeq) +
            " too far ahead of commitPoint.seq = " + std::to_string(checkpoint.seq)
        );
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

    if (r.client_id() != entry->client_id || r.client_seq() != entry->client_seq) {
        VLOG(5) << "Fail adding cert because mismatching request!";
        return false;
    }

    certs[seq] = std::make_shared<dombft::proto::Cert>(cert);
    return true;
}

const std::string &Log::getDigest() const
{
    if (log.empty()) {
        return;
    }
    return log.back().digest;
}

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

    checkpoint.toProto(*checkpointProto);

    for (const LogEntry &entry : log) {
        dombft::proto::LogEntry *entryProto = msg.add_log_entries();
        entry.toProto(*entryProto);
    }
}

const LogEntry &Log::getEntry(uint32_t seq)
{
    if (inRange(seq)) {
        uint32_t index = seq % MAX_SPEC_HIST;
        return log[index];
    } else {
        LOG(ERROR) << "Sequence number " << seq << " is out of range.";
        return nullptr;
    }
}

void Log::setEntry(uint32_t seq, std::shared_ptr<LogEntry> &entry)
{
    if (inRange(seq)) {
        uint32_t index = seq % MAX_SPEC_HIST;
        log[index] = entry;
    } else {
        LOG(ERROR) << "Sequence number " << seq << " is out of range.";
    }
}

// copies the entries at idx to idx + num, starting from startSeq
void Log::rightShiftEntries(uint32_t startSeq, uint32_t num)
{
    if (inRange(startSeq)) {
        std::vector<std::shared_ptr<LogEntry>> temp(nextSeq - startSeq);
        for (uint32_t i = startSeq; i < nextSeq; i++) {
            temp[i - startSeq] = log[i % MAX_SPEC_HIST];
        }
        for (uint32_t i = startSeq + num; i < nextSeq + num; i++) {
            log[i % MAX_SPEC_HIST] = temp[i - startSeq - num];
        }
        nextSeq += num;
    } else {
        LOG(ERROR) << "Sequence number " << startSeq << " is out of range.";
    }
}

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
    out << "CHECKPOINT " << l.checkpoint.seq << ": " << digest_to_hex(l.checkpoint.logDigest).substr(56) << " | ";
    uint32_t i = l.checkpoint.seq + 1;
    for (; i < l.nextSeq; i++) {
        int idx = i % MAX_SPEC_HIST;
        out << *l.log[idx];
    }
    return out;
}