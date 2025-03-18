#include "repair_utils.h"

#include "utils.h"

typedef std::pair<uint32_t, uint32_t> RequestId;
typedef std::map<RequestId, const dombft::proto::LogEntry *> ClientReqs;

ClientReqs getValidClientRequests(const dombft::proto::RepairProposal &repairProposal)
{
    uint32_t f = repairProposal.logs().size() / 2;

    // Compute all client requests in the proposal, so we can add them to the log suffix deterministically
    ClientReqs ret;

    std::map<RequestId, std::map<std::string, uint32_t>> requestCounts;

    for (int i = 0; i < repairProposal.logs().size(); i++) {
        auto &log = repairProposal.logs()[i];

        for (const dombft::proto::LogEntry &entry : log.log_entries()) {
            std::pair<uint32_t, uint32_t> key = {entry.client_id(), entry.client_seq()};

            requestCounts[key][entry.digest()]++;

            if (requestCounts[key][entry.digest()] >= f + 1) {
                ret[key] = &entry;
            }
        }
    }

    return ret;
}

bool getLogSuffixFromProposal(const dombft::proto::RepairProposal &repairProposal, LogSuffix &logSuffix)
{
    LOG(INFO) << "Start getLogSuffixFromProposal";

    uint32_t f = repairProposal.logs().size() / 2;

    // TODO verify messages so this isn't unsafe
    uint32_t maxCheckpointSeq = 0;

    // First find highest checkpoint
    for (auto &log : repairProposal.logs()) {
        if (log.checkpoint().seq() >= maxCheckpointSeq) {
            logSuffix.checkpoint = &log.checkpoint();
            logSuffix.checkpointReplica = log.replica_id();
            maxCheckpointSeq = log.checkpoint().seq();
        }
    }

    VLOG(4) << "Highest checkpoint is for seq=" << logSuffix.checkpoint->seq();

    // Find highest sequence with a cert
    // Idx of log we will use to match our logs to the repair agreed upon logs (up to cert)
    uint32_t logToUseIdx = 0;
    uint32_t logToUseSeq = 0;

    const dombft::proto::Cert *cert = nullptr;
    uint32_t maxCertSeq = 0;

    // get the max cert seq by comparing the seq in each of the included cert.
    // we have already verified these certs, so we can trust their seq numbers.
    for (int i = 0; i < repairProposal.logs().size(); i++) {
        const dombft::proto::RepairStart &repairLog = repairProposal.logs()[i];

        // Already included in checkpoint
        if (!repairLog.has_cert() || repairLog.cert().seq() <= logSuffix.checkpoint->seq())
            continue;

        if (repairLog.cert().seq() > maxCertSeq) {
            maxCertSeq = repairLog.cert().seq();
            logToUseIdx = i;
            cert = &repairLog.cert();
        }
    }

    if (cert != nullptr) {
        VLOG(4) << "Max cert found for seq=" << maxCertSeq << " c_id=" << cert->replies()[0].client_id()
                << " c_seq=" << cert->replies()[0].client_seq();
    } else {
        VLOG(4) << "No certs found!";
    }

    std::set<std::pair<uint32_t, uint32_t>> clientReqsAdded;

    // Add entries up to cert
    for (const dombft::proto::LogEntry &entry : repairProposal.logs()[logToUseIdx].log_entries()) {
        if (entry.seq() > maxCertSeq)
            break;

        clientReqsAdded.insert({entry.client_id(), entry.client_seq()});
        logSuffix.entries.push_back(&entry);
    }

    // Counts of matching digests for each seq coming after max cert
    std::map<uint32_t, std::map<std::string, uint32_t>> matchingEntries;

    // Find the common suffix after the max cert position
    for (int i = 0; i < repairProposal.logs().size(); i++) {
        auto &log = repairProposal.logs()[i];
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
    for (const dombft::proto::LogEntry &entry : repairProposal.logs()[logToUseIdx].log_entries()) {
        if (entry.seq() <= maxCertSeq)
            continue;

        if (entry.seq() > logToUseSeq)
            break;

        if (clientReqsAdded.contains({entry.client_id(), entry.client_seq()}))
            continue;
        clientReqsAdded.insert({entry.client_id(), entry.client_seq()});

        logSuffix.entries.push_back(&entry);
    }

    ClientReqs remainingClientReqs = getValidClientRequests(repairProposal);

    // Remove all requests already in the log suffix
    for (const auto &entry : logSuffix.entries) {
        remainingClientReqs.erase({entry->client_id(), entry->client_seq()});
    }

    // Note if a client equivocates, then we just use whichever request this function gives us,
    // since it is deterministic. Instead we just take whichever has at least f + 1 matching digests

    uint32_t finalLogSeq = logToUseSeq;
    for (auto &[key, val] : remainingClientReqs) {
        const dombft::proto::LogEntry *entry = val;
        logSuffix.entries.push_back(entry);
        finalLogSeq++;
    }

    VLOG(4) << "Rest of client requestes added from seq=" << logToUseSeq << " to seq=" << finalLogSeq;

    return true;
}

std::vector<ClientRequest> getAbortedEntries(const LogSuffix &logSuffix, std::shared_ptr<Log> log)
{
    // Save any client requests that we are aborting, in case we need to re-execute them
    std::vector<ClientRequest> ret;
    std::set<RequestId> keptReqs;

    for (const dombft::proto::LogEntry *entry : logSuffix.entries) {
        keptReqs.insert({entry->client_id(), entry->client_seq()});
    }

    for (uint32_t seq = logSuffix.checkpoint->seq() + 1; seq < log->getNextSeq(); seq++) {
        const LogEntry &entry = log->getEntry(seq);
        RequestId key = {entry.client_id, entry.client_seq};

        if (!keptReqs.contains(key)) {
            ClientRequest req;
            req.clientId = entry.client_id;
            req.clientSeq = entry.client_seq;
            req.requestData = entry.request;

            ret.push_back(req);
        }
    }
    return ret;
}

void applySuffix(LogSuffix &logSuffix, std::shared_ptr<Log> log)
{
    // This should only be called when current checkpoint is consistent with repair checkpoint
    LOG(INFO) << "checkpoint seq=" << logSuffix.checkpoint->seq()
              << " stable checkpoint seq=" << log->getStableCheckpoint().seq;
    assert(logSuffix.checkpoint->seq() == log->getStableCheckpoint().seq);

    // First sequence to apply is right after checkpoint
    uint32_t seq = logSuffix.checkpoint->seq() + 1;
    uint32_t idx = 0;

    // Reset the client record to the one in the checkpoint so we can rebuild it
    log->getClientRecord() = log->getStableCheckpoint().clientRecord_;

    LOG(INFO) << "Start applySuffixAfterCheckpoint";
    // 2 Skip the entries that are already in the log (consistent)
    for (; idx < logSuffix.entries.size() && seq < log->getNextSeq(); idx++) {
        const dombft::proto::LogEntry *entry = logSuffix.entries[idx];

        // 2.2 If inconsistency is detected, abort own entries after the last consistent entry
        if (log->getDigest(seq) != entry->digest()) {
            break;
        }

        VLOG(6) << "Skipping c_id=" << entry->client_id() << " c_seq=" << entry->client_seq()
                << " since already in log at seq=" << seq;

        log->getClientRecord().update(entry->client_id(), entry->client_seq());
        seq++;
    }

    LOG(INFO) << "Aborting own entries from seq=" << seq;
    log->abort(seq);

    // Step3. Apply entries after inconsistency is detected or suffix is longer than own log
    for (; idx < logSuffix.entries.size(); idx++) {
        LOG(INFO) << "Applying entry at seq=" << seq << " log next seq=" << log->getNextSeq();

        assert(seq == log->getNextSeq());
        const dombft::proto::LogEntry *entry = logSuffix.entries[idx];
        uint32_t clientId = entry->client_id();
        uint32_t clientSeq = entry->client_seq();

        std::string result;
        if (!log->addEntry(entry->client_id(), clientSeq, entry->request(), result)) {
            LOG(ERROR) << "Failure to add log entry!";
            continue;
        }

        VLOG(1) << "PERF event=repair_execute replica_id=" << logSuffix.replicaId << " seq=" << seq
                << " round=" << logSuffix.round << " client_id=" << clientId << " client_seq=" << entry->client_seq()
                << " digest=" << digest_to_hex(log->getDigest());
        seq++;
    }
}
