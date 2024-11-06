#include "fallback_utils.h"

#include "utils.h"

bool getLogSuffixFromProposal(const dombft::proto::FallbackProposal &fallbackProposal, LogSuffix &logSuffix)
{
    LOG(INFO) << "Start getLogSuffixFromProposal";

    uint32_t f = fallbackProposal.logs().size() / 2;

    // TODO verify messages so this isn't unsafe
    uint32_t maxCheckpointSeq = 0;

    // First find highest commit point
    const dombft::proto::CheckpointClientRecordsSet *tmpClientRecordsSetPtr;
    for (auto &log : fallbackProposal.logs()) {
        if (log.checkpoint().seq() >= maxCheckpointSeq) {
            logSuffix.checkpoint = &log.checkpoint();
            maxCheckpointSeq = log.checkpoint().seq();
            tmpClientRecordsSetPtr = &log.client_records_set();
        }
    }
    getClientRecordsFromProto(*tmpClientRecordsSetPtr, logSuffix.clientRecords);
    VLOG(4) << "Highest checkpoint is for seq=" << logSuffix.checkpoint->seq();

    // Find highest request with a cert
    // Idx of log we will use to match our logs to the fallback agreed upon logs (up to cert)
    uint32_t logToUseIdx = 0;
    uint32_t logToUseSeq = 0;

    const dombft::proto::Cert *cert = nullptr;
    uint32_t maxCertSeq = 0;

    for (int i = 0; i < fallbackProposal.logs().size(); i++) {
        auto &fallbackLog = fallbackProposal.logs()[i];
        // TODO verify each log
        for (const dombft::proto::LogEntry &entry : fallbackLog.log_entries()) {
            if (!entry.has_cert())
                continue;

            // Already included in checkpoint
            if (entry.seq() <= logSuffix.checkpoint->seq())
                continue;

            if (entry.cert().instance() < fallbackProposal.instance() - 1)
                continue;

            // TODO verify cert
            // If the cert doesn't match the log, a bad log suffix could be injected
            if (entry.seq() > maxCertSeq) {
                VLOG(4) << "Cert found for seq=" << entry.seq() 
                        << " c_id=" << entry.cert().cert_entries(0).batched_reply().replies(0).client_id()
                        << " c_seq=" << entry.cert().cert_entries(0).batched_reply().replies(0).client_seq();


                cert = &entry.cert();
                logToUseIdx = i;
                logToUseSeq = entry.seq();
                maxCertSeq = entry.seq();
            }
        }
    }

    if (cert != nullptr)
        VLOG(4) << "Max cert found for seq=" << maxCertSeq 
                << " c_id=" << cert->cert_entries(0).batched_reply().replies(0).client_id()
                << " c_seq=" << cert->cert_entries(0).batched_reply().replies(0).client_seq();
    else
        VLOG(4) << "No certs found!";

    // Add entries up to cert
    for (const dombft::proto::LogEntry &entry : fallbackProposal.logs()[logToUseIdx].log_entries()) {
        if (entry.seq() > logToUseSeq)
            break;

        logSuffix.entries.push_back(&entry);
    }

    // Counts of matching digests for each seq coming after max cert
    std::map<uint32_t, std::map<std::string, uint32_t>> matchingEntries;
    // Track latest applied clientSequence number
    // TODO we make some assumptions about client requests coming in order, which aren't ideal.
    std::map<uint32_t, uint32_t> maxMatchClientSeq;

    // TODO save this info in the checkpoint
    std::map<uint32_t, std::map<uint32_t, const dombft::proto::LogEntry *>> clientReqs;

    // Find the common suffix after the max cert position
    for (int i = 0; i < fallbackProposal.logs().size(); i++) {
        auto &log = fallbackProposal.logs()[i];
        // TODO verify each checkpoint
        for (const dombft::proto::LogEntry &entry : log.log_entries()) {
            if (entry.seq() <= maxCertSeq)
                continue;

            matchingEntries[entry.seq()][entry.digest()]++;
            clientReqs[entry.client_id()][entry.client_seq()] = &entry;

            if (matchingEntries[entry.seq()][entry.digest()] == f + 1) {
                VLOG(6) << "f + 1 matching digests found for seq=" << entry.seq() << " c_id=" << entry.client_id()
                        << " c_seq=" << entry.client_seq();

                maxMatchClientSeq[entry.client_id()] =
                    std::max(maxMatchClientSeq[entry.client_id()], entry.client_seq());
                logToUseIdx = i;
                logToUseSeq = entry.seq();
            }
        }
    }

    VLOG(4) << "f + 1 matching digests found up from maxCertSeq=" << maxCertSeq << " seq=" << logToUseSeq;

    // Add entries with f + 1 entries
    for (const dombft::proto::LogEntry &entry : fallbackProposal.logs()[logToUseIdx].log_entries()) {
        if (entry.seq() <= maxCertSeq)
            continue;

        if (entry.seq() > logToUseSeq)
            break;

        logSuffix.entries.push_back(&entry);
    }

    // Add rest of client requests in deterministic order lexicographically by (client_id, client_seq)
    for (auto &[c_id, reqs] : clientReqs) {
        for (auto &[c_seq, entry] : reqs) {
            // Already matched and added
            if (c_seq <= maxMatchClientSeq[c_id])
                continue;

            logSuffix.entries.push_back(entry);
        }
    }
    return true;
}

bool applySuffixToLog(LogSuffix &logSuffix, const std::shared_ptr<Log> &log)
{
    LOG(INFO) << "Applying checkpoint";

    // TODO this only works with our basic counter because app_digest == counter!

    const dombft::proto::LogCheckpoint *checkpoint = logSuffix.checkpoint;
    LogCheckpoint &myCheckpoint = log->checkpoint;

    std::string myCheckpointDigest((char *) myCheckpoint.logDigest, SHA256_DIGEST_LENGTH);

    if (checkpoint->log_digest() != myCheckpointDigest && checkpoint->seq() > myCheckpoint.seq) {
        log->app_->applySnapshot(checkpoint->app_digest());
        log->nextSeq = checkpoint->seq() + 1;

        myCheckpoint.seq = checkpoint->seq();
        memcpy(myCheckpoint.appDigest, checkpoint->app_digest().c_str(), checkpoint->app_digest().size());
        memcpy(myCheckpoint.logDigest, checkpoint->log_digest().c_str(), checkpoint->log_digest().size());
        myCheckpoint.cert = checkpoint->cert();

        for (int i = 0; i < checkpoint->commits().size(); i++) {
            auto &commit = checkpoint->commits()[i];

            myCheckpoint.commitMessages[commit.replica_id()] = commit;
            myCheckpoint.signatures[commit.replica_id()] = checkpoint->signatures()[i];
        }

        LOG(INFO) << "Applying checkpoint seq=" << checkpoint->seq()
                  << " digest=" << digest_to_hex(myCheckpoint.logDigest).substr(56);
    }

    uint32_t seq = checkpoint->seq();
    uint32_t idx = 0;
    dombft::ClientRecords &clientRecords = logSuffix.clientRecords;
    // skip the entries that are already in the log
    for (; idx < logSuffix.entries.size(); idx++) {
        seq++;   // First sequence to apply is right after checkpoint
        assert(seq < log->nextSeq);
        const dombft::proto::LogEntry *entry = logSuffix.entries[idx];

        std::shared_ptr<LogEntry> myEntry = log->getEntry(seq);
        std::string myDigest(myEntry->digest, myEntry->digest + SHA256_DIGEST_LENGTH);
        // mismatch found, rollback
        if (myDigest != entry->digest()) {
            log->nextSeq = seq;
            log->app_->abort(seq - 1);
            break;
        }
        // the skipped entries cannot be duplicates
        assert(updateRecordWithSeq(clientRecords[entry->client_id()], entry->client_seq()));
        VLOG(6) << "Skipping c_id=" << entry->client_id() << " c_seq=" << entry->client_seq()
                << " since already in log at seq=" << seq;
    }

    for (; idx < logSuffix.entries.size(); idx++) {
        assert(seq == log->nextSeq);
        const dombft::proto::LogEntry *entry = logSuffix.entries[idx];
        uint32_t clientId = entry->client_id();
        uint32_t clientSeq = entry->client_seq();

        if (!log->canAddEntry()) {
            LOG(INFO) << "nextSeq=" << log->nextSeq << " too far ahead of commitPoint.seq=" << log->checkpoint.seq;
            break;
        }
        if (!updateRecordWithSeq(clientRecords[clientId], clientSeq)) {
            LOG(INFO) << "Dropping request c_id=" << entry->client_id() << " c_seq=" << entry->client_seq()
                      << " due to duplication in applying suffix!";
            continue;
        }
        clientRecords[clientId].instance_ = logSuffix.instance;

        std::string result;
        if (!log->addEntry(entry->client_id(), clientSeq, entry->request(), result)) {
            LOG(ERROR) << "Failure to add log entry!";
        }

        // TODO(Hao): should it reply to client?
        VLOG(1) << "PERF event=fallback_execute replica_id=" << logSuffix.replicaId << " seq=" << seq
                << " instance=" << logSuffix.instance << " client_id=" << clientId
                << " client_seq=" << entry->client_seq() << " digest=" << digest_to_hex(log->getDigest()).substr(56);
        seq++;
    }
    return true;
}
