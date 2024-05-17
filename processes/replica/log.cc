#include <log.h>

Log::Log()
    : nextSeq(1), lastExecuted(0)
{
}

void Log::addEntry(uint32_t c_id, uint32_t c_seq,
                   byte *req, uint32_t req_len)
{
    uint32_t prevSeq = (nextSeq + log.size() - 1) % log.size();
    byte *prevDigest = log[prevSeq]->digest;

    log[nextSeq % log.size()] = std::make_unique<LogEntry>(nextSeq, c_id, c_seq, req, req_len, prevDigest);
}

bool Log::addAndExecuteEntry(uint32_t c_id, uint32_t c_seq,
                             byte *req, uint32_t req_len)
{
    if (lastExecuted != nextSeq - 1) {
        return false;
    }

    addEntry(c_id, c_seq, req, req_len);

    // TODO execute

    lastExecuted++;
    return true;
}

void Log::addCert(uint32_t seq)
{
    // TODO
}