#include "log.h"

#include "utils.h"

#include <glog/logging.h>

LogEntry::LogEntry()
    : seq(0)
    , client_id(0)
    , client_seq(0)
    , raw_request(nullptr)
    , raw_result(nullptr)
{
    memset(digest, 0, SHA256_DIGEST_LENGTH);
}

LogEntry::LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq, byte *req, uint32_t req_len, byte *prev_digest)
    : seq(s)
    , client_id(c_id)
    , client_seq(c_seq)
    , raw_request((byte *) malloc(req_len))   // Manually allocate some memory to store the request
    , raw_result(nullptr)
    , request_len(req_len)
    , result_len(0)
{
    memcpy(raw_request, req, req_len);

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    SHA256_Update(&ctx, &seq, sizeof(seq));
    SHA256_Update(&ctx, &client_id, sizeof(client_id));
    SHA256_Update(&ctx, &client_seq, sizeof(client_seq));
    SHA256_Update(&ctx, prev_digest, SHA256_DIGEST_LENGTH);
    SHA256_Update(&ctx, raw_request, req_len);
    SHA256_Final(digest, &ctx);
}

LogEntry::~LogEntry()
{
    if (raw_request != nullptr) {
        free(raw_request);
    }
    if (raw_result != nullptr) {
        free(raw_result);
    }
}

std::ostream &operator<<(std::ostream &out, const LogEntry &le)
{
    out << le.seq << ": (" << le.client_id << ", " << le.client_seq << ") " << digest_to_hex(le.digest).substr(56)
        << " | ";
    return out;
}

Log::Log()
    : nextSeq(1)
    , lastExecuted(0)
{
    // Zero initialize all the entries
    // TODO: there's probably a better way to handle this
    for (uint32_t i = 0; i < log.size(); i++) {
        log[i] = std::make_unique<LogEntry>();
    }
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq, byte *req, uint32_t req_len)
{
    uint32_t prevSeqIdx = (nextSeq + log.size() - 1) % log.size();
    byte *prevDigest = log[prevSeqIdx]->digest;

    if (nextSeq > commitPoint.seq + MAX_SPEC_HIST) {
        LOG(INFO) << "nextSeq=" << nextSeq << " too far ahead of commitPoint.seq=" << commitPoint.seq;
        return false;
    }

    log[nextSeq % log.size()] = std::make_unique<LogEntry>(nextSeq, c_id, c_seq, req, req_len, prevDigest);

    VLOG(4) << "Adding new entry at seq=" << nextSeq << " c_id=" << c_id << " c_seq=" << c_seq;
    nextSeq++;

    return true;
}

bool Log::executeEntry(uint32_t seq)
{
    if (lastExecuted != seq - 1) {
        return false;
    }

    // TODO execute and get result back.
    lastExecuted++;
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

// Create a new commit point given the existence of a certificate at seq
bool Log::createCommitPoint(uint32_t seq)
{
    // if (certs.count(seq) == 0)
    // {
    //     LOG(ERROR) << "Attempt to create a commit point at seq " << seq
    //                << " but no cert exists!";
    // }
    LOG(INFO) << "Creating tentative commit point for " << seq;

    tentativeCommitPoint = LogCheckpoint();   // TODO use a constructor?

    tentativeCommitPoint->seq = seq;

    // Note, CERT, logDigest, appDigest get added later
    // TODO maybe don't create one without these?
    memset(tentativeCommitPoint->logDigest, 0, SHA256_DIGEST_LENGTH);
    memset(tentativeCommitPoint->appDigest, 0, SHA256_DIGEST_LENGTH);

    tentativeCommitPoint->commitMessages.clear();
    tentativeCommitPoint->signatures.clear();

    LOG(INFO) << "Created tentative commit point for " << seq;

    return true;
}

bool Log::addCommitMessage(const dombft::proto::Commit &commit, byte *sig, int sigLen)
{
    int from = commit.replica_id();

    if (!tentativeCommitPoint.has_value()) {
        LOG(ERROR) << "Trying to add commit message to empty commit point!";
        return false;
    }

    // TODO check match?

    tentativeCommitPoint->commitMessages[from] = commit;
    tentativeCommitPoint->signatures[from] = std::string(sig, sig + sigLen);

    return true;
}

bool Log::commitCommitPoint()
{
    if (!tentativeCommitPoint.has_value()) {
        LOG(ERROR) << "Trying to commit with empty tentative commit point!";
        return false;
    }

    // TODO truncate log/commit application state

    commitPoint = tentativeCommitPoint.value();
    tentativeCommitPoint.reset();

    return true;
}

void Log::toProto(dombft::proto::FallbackStart &msg)
{
    dombft::proto::LogCheckpoint *checkpoint = msg.mutable_checkpoint();

    if (commitPoint.seq > 0) {
        checkpoint->set_seq(commitPoint.seq);
        checkpoint->set_app_digest((const char *) commitPoint.appDigest, SHA256_DIGEST_LENGTH);
        checkpoint->set_log_digest((const char *) commitPoint.logDigest, SHA256_DIGEST_LENGTH);

        for (auto x : commitPoint.commitMessages) {
            (*checkpoint->add_commits()) = x.second;
            checkpoint->add_signatures(commitPoint.signatures[x.first]);
        }

        if (!commitPoint.cert.has_value()) {
            LOG(ERROR) << "commitPoint cert is null!";
        }

        (*checkpoint->mutable_cert()) = commitPoint.cert.value();
    }

    for (int i = commitPoint.seq + 1; i < nextSeq; i++) {
        dombft::proto::LogEntry *entryProto = msg.add_log_entries();
        LogEntry &entry = *log[i % log.size()];

        assert(i == entry.seq);

        if (certs.count(i)) {
            (*checkpoint->mutable_cert()) = *certs[i];
        }

        entryProto->set_seq(i);
        entryProto->set_client_id(entry.client_id);
        entryProto->set_client_seq(entry.client_seq);
        entryProto->set_digest(entry.digest, SHA256_DIGEST_LENGTH);
        entryProto->set_request(entry.raw_request, entry.request_len);
        entryProto->set_result(entry.raw_result, entry.result_len);
    }
}

std::ostream &operator<<(std::ostream &out, const Log &l)
{
    // go from nextSeq - MAX_SPEC_HIST, which traverses the whole buffer
    // starting from the oldest;
    int i = l.nextSeq - MAX_SPEC_HIST;
    i = i < 0 ? 0 : i;   // std::max isn't playing nice
    for (; i < l.nextSeq; i++) {
        int seq = i % MAX_SPEC_HIST;
        out << *l.log[seq];
    }
    return out;
}
