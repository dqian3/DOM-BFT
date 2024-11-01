#include "log.h"

#include "utils.h"

#include <glog/logging.h>

LogEntry::LogEntry()
    : seq(0)
    , client_id(0)
    , client_seq(0)
{
    memset(digest, 0, SHA256_DIGEST_LENGTH);
}

LogEntry::LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, const std::string &req, const byte *prev_digest)
    : seq(s)
    , client_id(c_id)
    , client_seq(c_seq)
    , request(req)   // Manually allocate some memory to store the request
{
    result = "";

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    SHA256_Update(&ctx, &seq, sizeof(seq));
    SHA256_Update(&ctx, &client_id, sizeof(client_id));
    SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
    SHA256_Update(&ctx, prev_digest, SHA256_DIGEST_LENGTH);
    SHA256_Update(&ctx, request.c_str(), request.length());
    SHA256_Final(digest, &ctx);
}

void LogEntry::updateDigest(const byte *prev_digest)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    SHA256_Update(&ctx, &seq, sizeof(seq));
    SHA256_Update(&ctx, &client_id, sizeof(client_id));
    SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
    SHA256_Update(&ctx, prev_digest, SHA256_DIGEST_LENGTH);
    SHA256_Update(&ctx, request.c_str(), request.length());
    SHA256_Final(digest, &ctx);
}

LogEntry::~LogEntry() {}

std::ostream &operator<<(std::ostream &out, const LogEntry &le)
{
    out << le.seq << ": (" << le.client_id << ", " << le.client_seq << ") " << digest_to_hex(le.digest).substr(56)
        << " | ";
    return out;
}

Log::Log(std::shared_ptr<Application> app)
    : nextSeq(1)
    , lastExecuted(0)
    , app_(std::move(app))
{
    LOG(INFO) << "Initializing log entries";
    // Zero initialize all the entries
    // TODO: there's probably a better way to handle this
    for (uint32_t i = 0; i < log.size(); i++) {
        log[i] = std::make_unique<LogEntry>();
    }
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res)
{
    assert(nextSeq);
    byte *prevDigest = nullptr;
    if (nextSeq - 1 == checkpoint.seq) {
        VLOG(4) << "Using checkpoint digest as previous for seq=" << nextSeq;
        prevDigest = checkpoint.logDigest;
    } else {
        prevDigest = log[(nextSeq - 1) % log.size()]->digest;
    }

    if (!canAddEntry()) {
        throw std::runtime_error(
            "nextSeq = " + std::to_string(nextSeq) +
            " too far ahead of commitPoint.seq = " + std::to_string(checkpoint.seq)
        );
    }

    log[nextSeq % log.size()] = std::make_unique<LogEntry>(nextSeq, c_id, c_seq, req, prevDigest);

    res = app_->execute(req, nextSeq);
    log[nextSeq % log.size()]->result = res;

    VLOG(4) << "Adding new entry at seq=" << nextSeq << " c_id=" << c_id << " c_seq=" << c_seq
            << " digest=" << digest_to_hex(log[nextSeq % log.size()]->digest).substr(56);

    nextSeq++;
    return true;
}

void Log::addCert(uint32_t seq, const dombft::proto::Cert &cert)
{
    certs[seq] = std::make_shared<dombft::proto::Cert>(cert);
}

const byte *Log::getDigest() const
{
    if (nextSeq == 0) {
        return nullptr;
    }

    uint32_t prevSeq = (nextSeq + log.size() - 1) % log.size();
    return log[prevSeq]->digest;
}

const byte *Log::getDigest(uint32_t seq) const
{
    if (!inRange(seq)) {
        LOG(ERROR) << "Tried to access digest of seq=" << seq << " but nextSeq=" << nextSeq;
        return nullptr;
    }
    uint32_t seqIdx = (seq + log.size()) % log.size();
    return log[seqIdx]->digest;
}

void Log::toProto(dombft::proto::FallbackStart &msg)
{
    dombft::proto::LogCheckpoint *checkpointProto = msg.mutable_checkpoint();

    if (checkpoint.seq > 0) {
        checkpointProto->set_seq(checkpoint.seq);
        checkpointProto->set_app_digest((const char *) checkpoint.appDigest, SHA256_DIGEST_LENGTH);
        checkpointProto->set_log_digest((const char *) checkpoint.logDigest, SHA256_DIGEST_LENGTH);

        for (auto x : checkpoint.commitMessages) {
            (*checkpointProto->add_commits()) = x.second;
            checkpointProto->add_signatures(checkpoint.signatures[x.first]);
        }

        (*checkpointProto->mutable_cert()) = checkpoint.cert;
    } else {
        checkpointProto->set_seq(0);
        checkpointProto->set_app_digest("");
        checkpointProto->set_log_digest("");
    }

    for (int i = checkpoint.seq + 1; i < nextSeq; i++) {
        dombft::proto::LogEntry *entryProto = msg.add_log_entries();
        LogEntry &entry = *log[i % log.size()];

        assert(i == entry.seq);

        if (certs.count(i)) {
            (*entryProto->mutable_cert()) = *certs[i];
        }

        entryProto->set_seq(i);
        entryProto->set_client_id(entry.client_id);
        entryProto->set_client_seq(entry.client_seq);
        entryProto->set_digest(entry.digest, SHA256_DIGEST_LENGTH);
        entryProto->set_request(entry.request);
        entryProto->set_result(entry.result);
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

std::shared_ptr<LogEntry> Log::getEntry(uint32_t seq)
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