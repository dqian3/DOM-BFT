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

LogEntry::LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, const std::string &req, byte *prev_digest)
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
    // TODO add this back in, currently fallback doesnt' work with this
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

Log::Log(std::unique_ptr<Application> app)
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

    if (nextSeq > checkpoint.seq + MAX_SPEC_HIST) {
        LOG(INFO) << "nextSeq=" << nextSeq << " too far ahead of commitPoint.seq=" << checkpoint.seq;
        // TODO error out properly
        return false;
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
    if (seq + MAX_SPEC_HIST < nextSeq) {
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
            (*checkpointProto->mutable_cert()) = *certs[i];
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
    int i = l.nextSeq - MAX_SPEC_HIST;
    i = i < 0 ? 0 : i;   // std::max isn't playing nice
    for (; i < l.nextSeq; i++) {
        int idx = i % MAX_SPEC_HIST;

        // Skip any committed/truncated logs
        if (l.log[idx]->seq <= l.checkpoint.seq)
            continue;

        out << *l.log[idx];
    }
    return out;
}

std::shared_ptr<LogEntry> Log::getEntry(uint32_t seq)
{
    if (seq < nextSeq && (seq >= nextSeq - MAX_SPEC_HIST || seq < MAX_SPEC_HIST)) {
        uint32_t index = seq % MAX_SPEC_HIST;
        return log[index];
    } else {
        return nullptr;
    }
}

void Log::commit(uint32_t seq)
{
    if (seq < nextSeq && (seq >= nextSeq - MAX_SPEC_HIST || seq < MAX_SPEC_HIST)) {
        app_.get()->commit(seq);
    } else {
        LOG(ERROR) << "Sequence number " << seq << " is out of range.";
    }
}
