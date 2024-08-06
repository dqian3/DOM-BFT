#include "log.h"

#include <glog/logging.h>

using namespace dombft::proto;

LogEntry::LogEntry()
    : seq(0), client_id(0), client_seq(0), raw_request(nullptr)
{
    raw_result = "";
    memset(digest, 0, SHA256_DIGEST_LENGTH);
}

LogEntry::LogEntry(uint32_t s, uint32_t c_id, uint32_t c_seq,
                   byte *req, uint32_t req_len, byte *prev_digest)
    : seq(s), client_id(c_id), client_seq(c_seq), raw_request((byte *)malloc(req_len)) // Manually allocate some memory to store the request
      ,
     request_len(req_len), result_len(0)
{
    memcpy(raw_request, req, req_len);

    raw_result = "";

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
    if (raw_request != nullptr)
    {
        free(raw_request);
        raw_request = nullptr;
    }
    // if (raw_result != nullptr)
    // {
    //     free(raw_result);
    //     raw_result = nullptr;
    // }
}

std::ostream &operator<<(std::ostream &out, const LogEntry &le)
{
    out << le.seq << ": (" << le.client_id << " ," << le.client_seq << ")";
    return out;
}

Log::Log()
    : nextSeq(1), lastExecuted(0)
{
    // Zero initialize all the entries
    // TODO: there's probably a better way to handle this
    for (uint32_t i = 0; i < log.size(); i++)
    {
        log[i] = std::make_unique<LogEntry>();
    }
}

Log::Log(AppType app_type)
    : nextSeq(1), lastExecuted(0)
{
    LOG(INFO) << "Initializing log entry";
    // Zero initialize all the entries
    // TODO: there's probably a better way to handle this
    for (uint32_t i = 0; i < log.size(); i++)
    {
        log[i] = std::make_unique<LogEntry>();
    }

    LOG(INFO) << "log entry initialized";

    if (app_type == AppType::COUNTER) {
        LOG(INFO) << "Creating a counter application";
        app_ = std::make_unique<Counter>();
    } else {
        LOG(ERROR) << "Unsupported application type";
        exit(1);
    }

    LOG(INFO) << "App initialized";
}

bool Log::addEntry(uint32_t c_id, uint32_t c_seq,
                   byte *req, uint32_t req_len)
{
    uint32_t prevSeqIdx = (nextSeq + log.size() - 1) % log.size();
    byte *prevDigest = log[prevSeqIdx]->digest;

    if (nextSeq > commitPoint.seq + MAX_SPEC_HIST) {
        LOG(INFO) << "nextSeq=" << nextSeq << " too far ahead of commitPoint.seq=" << commitPoint.seq;
        return false;
    }

    log[nextSeq % log.size()] = std::make_unique<LogEntry>(nextSeq, c_id, c_seq, req, req_len, prevDigest);

    VLOG(4) << "Adding new entry at seq=" << nextSeq << " c_id=" << c_id
            << " c_seq=" << c_seq;
    nextSeq++;

    return true;
}

bool Log::executeEntry(uint32_t seq)
{
    if (lastExecuted != seq - 1)
    {
        return false;
    }

    // TODO execute and get result back.
    lastExecuted++;
    return true;
}

bool Log::executeEntry(uint32_t seq, const ClientRequest &request, Reply &reply)
{

    if (lastExecuted != seq - 1)
    {
        return false;
    }

    LOG(INFO) << "Executing entry at seq=" << seq << " Sending to app layer";

    std::string appResponse = app_->execute(request.req_data(), seq);

    LOG(INFO) << "Got response from app layer: " << appResponse;

    reply.set_result(appResponse);


    getEntry(seq)->raw_result = appResponse;
    // TODO put the exeuction digest to the log entry as well, may need to add a field in the logentry struct. 

    // TODO execute and get result back.
    lastExecuted++;

    return true;

}

void Log::addCert(uint32_t seq, const Cert &cert)
{
    certs[seq] = std::make_unique<Cert>(cert);
}

const byte *Log::getDigest() const
{
    if (nextSeq == 0)
    {
        return nullptr;
    }
    uint32_t prevSeq = (nextSeq + log.size() - 1) % log.size();
    return log[prevSeq]->digest;
}

const byte *Log::getDigest(uint32_t seq) const
{
    if (seq + MAX_SPEC_HIST < nextSeq)
    {
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

    tentativeCommitPoint = LogCommitPoint(); // TODO use a constructor?

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

    if (!tentativeCommitPoint.has_value())
    {
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
    if (!tentativeCommitPoint.has_value())
    {
        LOG(ERROR) << "Trying to commit with empty tentative commit point!";
        return false;  
    }

    commitPoint = tentativeCommitPoint.value();
    tentativeCommitPoint.reset();

    LOG(INFO) << "New Stable Commit Point: " << commitPoint.seq;

    return true;
}



std::ostream &operator<<(std::ostream &out, const Log &l)
{
    // go from nextSeq - MAX_SPEC_HIST, which traverses the whole buffer
    // starting from the oldest;
    for (uint32_t i = l.nextSeq - MAX_SPEC_HIST; i < l.nextSeq; i++)
    {
        int seq = i % MAX_SPEC_HIST;
        out << l.log[seq].get();
    }
    return out;
}

LogEntry* Log::getEntry(uint32_t seq) {
    if (seq < nextSeq && (seq >= nextSeq - MAX_SPEC_HIST || seq < MAX_SPEC_HIST)) {
        uint32_t index = seq % MAX_SPEC_HIST;
        return log[index].get();
    } else {
        LOG(ERROR) << "Sequence number " << seq << " is out of range.";
        return nullptr;
    }
}

void Log::commit(uint32_t seq) {
    if (seq < nextSeq && (seq >= nextSeq - MAX_SPEC_HIST || seq < MAX_SPEC_HIST)) {
        app_.get()->commit(seq);
    } else {
        LOG(ERROR) << "Sequence number " << seq << " is out of range.";
    }
}