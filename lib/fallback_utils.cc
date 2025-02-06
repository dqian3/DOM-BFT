#include "fallback_utils.h"

#include "utils.h"

bool getLogSuffixFromProposal(const dombft::proto::FallbackProposal &fallbackProposal, LogSuffix &logSuffix)
{
    LOG(INFO) << "Start getLogSuffixFromProposal";

    uint32_t f = fallbackProposal.logs().size() / 2;

    // TODO verify messages so this isn't unsafe
    uint32_t maxCheckpointSeq = 0;

    // First find highest commit point
    const dombft::proto::ClientRecord *checkpointClientRecord = nullptr;
    for (auto &log : fallbackProposal.logs()) {
        if (log.checkpoint().seq() >= maxCheckpointSeq) {
            logSuffix.checkpoint = &log.checkpoint();
            maxCheckpointSeq = log.checkpoint().seq();
            checkpointClientRecord = &log.client_record();
        }
    }
    logSuffix.clientRecord = *checkpointClientRecord;

    VLOG(4) << "Highest checkpoint is for seq=" << logSuffix.checkpoint->seq();

    // Find highest sequence with a cert
    // Idx of log we will use to match our logs to the fallback agreed upon logs (up to cert)
    uint32_t logToUseIdx = 0;
    uint32_t logToUseSeq = 0;

    const dombft::proto::Cert *cert = nullptr;
    uint32_t maxCertSeq = 0;

    // get the max cert seq by comparing the seq in each of the included cert.
    // we have already verified these certs, so we can trust their seq numbers.
    for (int i = 0; i < fallbackProposal.logs().size(); i++) {
        const dombft::proto::FallbackStart &fallbackLog = fallbackProposal.logs()[i];

        // Already included in checkpoint
        if (!fallbackLog.has_cert() || fallbackLog.cert().seq() <= logSuffix.checkpoint->seq())
            continue;

        if (fallbackLog.cert().seq() > maxCertSeq) {
            maxCertSeq = fallbackLog.cert().seq();
            logToUseIdx = i;
            cert = &fallbackLog.cert();
        }
    }

    if (cert != nullptr) {
        VLOG(4) << "Max cert found for seq=" << maxCertSeq << " c_id=" << cert->replies()[0].client_id()
                << " c_seq=" << cert->replies()[0].client_seq();
    } else {
        VLOG(4) << "No certs found!";
    }

    // Add entries up to cert
    for (const dombft::proto::LogEntry &entry : fallbackProposal.logs()[logToUseIdx].log_entries()) {
        if (entry.seq() > maxCertSeq)
            break;

        logSuffix.entries.push_back(&entry);
    }

    // Counts of matching digests for each seq coming after max cert
    std::map<uint32_t, std::map<std::string, uint32_t>> matchingEntries;

    // Find the common suffix after the max cert position
    for (int i = 0; i < fallbackProposal.logs().size(); i++) {
        auto &log = fallbackProposal.logs()[i];
        // TODO verify each checkpoint
        for (const dombft::proto::LogEntry &entry : log.log_entries()) {
            if (entry.seq() <= maxCertSeq)
                continue;

            matchingEntries[entry.seq()][entry.digest()]++;

            if (matchingEntries[entry.seq()][entry.digest()] == f + 1) {
                VLOG(6) << "f + 1 matching digests found for seq=" << entry.seq() << " c_id=" << entry.client_id()
                        << " c_seq=" << entry.client_seq();

                logToUseIdx = i;
                logToUseSeq = entry.seq();
            }
        }
    }

    VLOG(4) << "f + 1 matching digests found from maxCertSeq=" << maxCertSeq << " to seq=" << logToUseSeq;

    // Add entries with f + 1 entries
    for (const dombft::proto::LogEntry &entry : fallbackProposal.logs()[logToUseIdx].log_entries()) {
        if (entry.seq() <= maxCertSeq)
            continue;

        if (entry.seq() > logToUseSeq)
            break;

        logSuffix.entries.push_back(&entry);
    }

    // Compute all client requests and which ones are already in suffix
    // and add rest of client requests in deterministic order lexicographically by (client_id, client_seq)

    std::map<uint32_t, std::map<uint32_t, const dombft::proto::LogEntry *>> remainingClientReqs;

    for (int i = 0; i < fallbackProposal.logs().size(); i++) {
        auto &log = fallbackProposal.logs()[i];
        // TODO verify each checkpoint
        for (const dombft::proto::LogEntry &entry : log.log_entries()) {
            remainingClientReqs[entry.client_id()][entry.client_seq()] = &entry;
        }
    }
    // Remove all requests already in the log suffix
    for (const auto &entry : logSuffix.entries) {
        remainingClientReqs[entry->client_id()].erase(entry->client_seq());
    }

    uint32_t finalLogSeq = logToUseSeq;
    for (auto &[c_id, reqs] : remainingClientReqs) {
        for (auto &[c_seq, entry] : reqs) {
            logSuffix.entries.push_back(entry);
            finalLogSeq++;
        }
    }

    VLOG(4) << "Rest of client requestes added from seq=" << logToUseSeq << " to seq=" << finalLogSeq;

    return true;
}

bool applySuffixWithCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log) { return true; }

bool applySuffixWithoutCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log) { return true; }

bool applySuffixAfterCheckpoint(LogSuffix &logSuffix, std::shared_ptr<Log> log)
{
    return true;
    // LOG(INFO) << "Applying checkpoint";

    // const dombft::proto::LogCheckpoint *checkpoint = logSuffix.checkpoint;
    // // Step1. Check if currently is behind suffix checkpoint
    // // *This is only called when current checkpoint is consistent
    // assert(checkpoint->seq() == log->stableCheckpoint.seq);

    // // Step2. Find the spot to start applying the suffix
    // // First sequence to apply is right after checkpoint
    // uint32_t seq = checkpoint->seq() + 1;
    // uint32_t idx = 0;
    // dombft::ClientRecord &clientRecords = logSuffix.clientRecords;
    // // 2.1 Skip the entries that are already in the log (consistent)
    // for (; idx < logSuffix.entries.size() && seq < log->nextSeq; idx++) {
    //     const dombft::proto::LogEntry *entry = logSuffix.entries[idx];
    //     std::shared_ptr<LogEntry> myEntry = log->getEntry(seq);
    //     std::string myDigest(myEntry->digest, myEntry->digest + SHA256_DIGEST_LENGTH);
    //     // 2.2 If inconsistency is detected, abort own entries after the last consistent entry
    //     if (myDigest != entry->digest()) {
    //         log->app_->abort(seq - 1);
    //         break;
    //     }
    //     // 2.3 Update client records & skip the duplicated entries
    //     if (!clientRecords[entry->client_id()].update(entry->client_seq())) {
    //         // TODO this is not ideal; we will reppeat client ops, but replicas will still be
    //         // consistent.
    //         VLOG(4) << "Skipped entry seq=" << seq << " c_id=" << entry->client_id() << " c_seq=" <<
    //         entry->client_seq()
    //                 << " because it is a duplicate!";
    //         continue;
    //     }
    //     VLOG(6) << "Skipping c_id=" << entry->client_id() << " c_seq=" << entry->client_seq()
    //             << " since already in log at seq=" << seq;
    //     seq++;
    // }

    // // Step3. Apply entries after inconsistency is detected or suffix is longer than own log
    // log->nextSeq = seq;
    // for (; idx < logSuffix.entries.size(); idx++) {
    //     assert(seq == log->nextSeq);
    //     const dombft::proto::LogEntry *entry = logSuffix.entries[idx];
    //     uint32_t clientId = entry->client_id();
    //     uint32_t clientSeq = entry->client_seq();

    //     if (!log->canAddEntry()) {
    //         LOG(INFO) << "nextSeq=" << log->getNextSeq()
    //                   << " too far ahead of commitPoint.seq=" << log->getStableCheckpoint().seq;
    //         break;
    //     }
    //     if (!clientRecords[entry->client_id()].update(clientSeq)) {
    //         LOG(INFO) << "Dropping request c_id=" << entry->client_id() << " c_seq=" << entry->client_seq()
    //                   << " due to duplication in applying suffix!";
    //         continue;
    //     }

    //     std::string result;
    //     if (!log->addEntry(entry->client_id(), clientSeq, entry->request(), result)) {
    //         LOG(ERROR) << "Failure to add log entry!";
    //     }

    //     VLOG(1) << "PERF event=fallback_execute replica_id=" << logSuffix.replicaId << " seq=" << seq
    //             << " instance=" << logSuffix.instance << " client_id=" << clientId
    //             << " client_seq=" << entry->client_seq() << " digest=" << digest_to_hex(log->getDigest()).substr(56);
    //     seq++;
    // }
    // return true;
}
