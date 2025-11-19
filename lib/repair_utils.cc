#include "repair_utils.h"

#include "utils.h"

#include "config/config_manager.h"

#include <cryptopp/filters.h>
#include <cryptopp/sha.h>

typedef std::pair<uint32_t, uint32_t> RequestId;
typedef std::map<RequestId, const dombft::proto::LogEntry *> ClientReqs;

ClientReqs getValidClientRequests(const dombft::proto::RepairProposal &repairProposal, uint32_t checkpointSeq)
{
    uint32_t f = dombft::ConfigManager::getInstance().getConfig().f;

    // Compute all client requests in the proposal, so we can add them to the log suffix deterministically
    ClientReqs ret;

    std::map<RequestId, std::map<std::string, uint32_t>> requestCounts;

    for (int i = 0; i < repairProposal.start_msgs().size(); i++) {
        auto &log = repairProposal.start_msgs()[i].log();

        for (const dombft::proto::LogEntry &entry : log.entries()) {
            if (entry.seq() <= checkpointSeq) {
                continue;
            }

            std::pair<uint32_t, uint32_t> key = {entry.client_id(), entry.client_seq()};

            requestCounts[key][entry.request()]++;

            if (requestCounts[key][entry.request()] >= f + 1) {
                ret[key] = &entry;
            }
        }
    }

    return ret;
}

bool getLogSuffixFromProposal(const dombft::proto::RepairProposal &repairProposal, LogSuffix &logSuffix)
{
    LOG(INFO) << "Start getLogSuffixFromProposal";

    if (VLOG_IS_ON(4)) {
        std::string replicaIds;
        for (auto &log : repairProposal.start_msgs()) {
            replicaIds += std::to_string(log.replica_id()) + " ";
        }
        VLOG(4) << "Replica ids in repairProposal: " << replicaIds;
    }

    const auto &config = dombft::ConfigManager::getInstance().getConfig();
    uint32_t f = config.f;
    uint32_t e = config.e;
    uint32_t n = dombft::ConfigManager::getInstance().getNumReplicas();

    uint32_t maxCheckpointSeq = 0;

    // TODO with fast path checkpoints which we added recently,
    // we should verify the if the maxCheckpoint is actually valid (i.e. matches f + 1 replicas)
    for (auto &startMsg : repairProposal.start_msgs()) {
        assert(startMsg.has_log());

        auto &log = startMsg.log();

        if (log.checkpoint().seq() >= maxCheckpointSeq) {
            logSuffix.checkpoint = &log.checkpoint();
            logSuffix.checkpointReplica = startMsg.replica_id();
            maxCheckpointSeq = log.checkpoint().seq();
        }
    }

    VLOG(4) << "Highest checkpoint is for seq=" << logSuffix.checkpoint->seq();

    // Verify the checkpoint matches at least f + e + 1 logs

    uint32_t numMatches = 0;
    for (int i = 0; i < repairProposal.start_msgs().size(); i++) {
        auto &msg = repairProposal.start_msgs()[i];
        auto &log = msg.log();

        if (msg.replica_id() == logSuffix.checkpointReplica)
            continue;

        std::string digest;
        if (log.checkpoint().seq() == logSuffix.checkpoint->seq()) {
            digest = log.checkpoint().log_digest();
        } else if (log.checkpoint().seq() + log.entries().size() >= logSuffix.checkpoint->seq()) {
            // The checkpoint can be matched from the entries
            const dombft::proto::LogEntry &entry =
                log.entries()[logSuffix.checkpoint->seq() - log.checkpoint().seq() - 1];
            digest = entry.digest();

        } else {
            // This log has a higher checkpoint than the agreed upon one, so we can't use it to match
            continue;
        }

        if (digest == logSuffix.checkpoint->log_digest()) {
            numMatches++;
            VLOG(6) << "Checkpoint from replica " << msg.replica_id() << " matches agreed upon checkpoint";
        }
    }

    if (numMatches < f + e + 1) {
        LOG(ERROR) << "Not enough matching checkpoints for seq=" << logSuffix.checkpoint->seq() << ", only "
                   << numMatches << " found";
        // TODO, we should actually just find the max checkpoint that has enough matches, instead of failing here,
        // but this will only happen if there are byzantine replicas, so I'm too lazy to implement this now.
        exit(1);
    }

    // Find highest sequence with a cert
    // Idx of log we will use to match our logs to the repair agreed upon logs (up to cert)
    uint32_t logToUseIdx = 0;
    uint32_t logToUseSeq = 0;

    const dombft::proto::Cert *cert = nullptr;
    uint32_t maxCertSeq = 0;

    // get the max cert seq by comparing the seq in each of the included cert.
    // we have already verified these certs, so we can trust their seq numbers.
    for (int i = 0; i < repairProposal.start_msgs().size(); i++) {
        const dombft::proto::RepairLog &repairLog = repairProposal.start_msgs()[i].log();

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

    // Add entries up to cert
    for (const dombft::proto::LogEntry &entry : repairProposal.start_msgs()[logToUseIdx].log().entries()) {
        if (entry.seq() <= logSuffix.checkpoint->seq())
            continue;

        if (entry.seq() > maxCertSeq)
            break;

        logSuffix.entries.push_back(&entry);
    }

    // Counts of matching digests for each seq coming after max cert
    std::map<uint32_t, std::map<std::string, uint32_t>> matchingEntries;

    // Find the common suffix after the max cert position
    for (int i = 0; i < repairProposal.start_msgs().size(); i++) {
        auto &log = repairProposal.start_msgs()[i].log();

        // TODO verify each checkpoint
        for (const dombft::proto::LogEntry &entry : log.entries()) {
            if (entry.seq() <= maxCertSeq || entry.seq() <= logSuffix.checkpoint->seq())
                continue;

            matchingEntries[entry.seq()][entry.digest()]++;

            if (matchingEntries[entry.seq()][entry.digest()] == f + e + 1) {
                VLOG(6) << "f + e + 1 matching digests found for seq=" << entry.seq() << " c_id=" << entry.client_id()
                        << " c_seq=" << entry.client_seq();

                logToUseIdx = i;
                logToUseSeq = entry.seq();
            }
        }
    }

    VLOG(4) << "f + e + 1 matching digests found from " << std::max(maxCheckpointSeq, maxCertSeq)
            << " to seq=" << logToUseSeq;

    // Add entries with f + 1 entries
    for (const dombft::proto::LogEntry &entry : repairProposal.start_msgs()[logToUseIdx].log().entries()) {
        if (entry.seq() <= maxCertSeq || entry.seq() <= logSuffix.checkpoint->seq())
            continue;

        if (entry.seq() > logToUseSeq)
            break;

        logSuffix.entries.push_back(&entry);
    }

    ClientReqs remainingClientReqs = getValidClientRequests(repairProposal, logSuffix.checkpoint->seq());

    // Remove all requests already in the log suffix or in the checkopint
    for (const auto &entry : logSuffix.entries) {
        remainingClientReqs.erase({entry->client_id(), entry->client_seq()});
    }

    if (VLOG_IS_ON(4)) {
        VLOG(4) << "Remaining client requests:";
        for (auto &[key, val] : remainingClientReqs) {
            VLOG(4) << "\tc_id=" << key.first << " c_seq=" << key.second;
        }
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

    ::ClientRecord checkpointClientRecord(logSuffix.checkpoint->client_record());
    for (const auto &entry : logSuffix.entries) {
        if (checkpointClientRecord.contains(entry->client_id(), entry->client_seq())) {
            LOG(ERROR) << "Client request c_id" << entry->client_id() << " c_seq=" << entry->client_seq()
                       << " already in checkpoint client record";
            continue;
        }

        assert(!checkpointClientRecord.contains(entry->client_id(), entry->client_seq()));
    }

    VLOG(4) << "Calculating digest for log suffix";
    // Calculate digest
    std::string prevDigest = logSuffix.checkpoint->log_digest();
    uint32_t seq = logSuffix.checkpoint->seq() + 1;

    for (const dombft::proto::LogEntry *e : logSuffix.entries) {
        ::LogEntry entry(seq, e->client_id(), e->client_seq(), e->request(), prevDigest);
        prevDigest = entry.digest;
        seq++;
    }
    logSuffix.logDigest = prevDigest;

    return true;
}

std::vector<ClientRequest> getAbortedEntries(const LogSuffix &logSuffix, std::shared_ptr<Log> log, uint32_t startSeq)
{
    // Save any client requests that we are aborting, in case we need to re-execute them
    std::vector<ClientRequest> ret;
    std::set<RequestId> keptReqs;

    for (const dombft::proto::LogEntry *entry : logSuffix.entries) {
        keptReqs.insert({entry->client_id(), entry->client_seq()});
    }

    startSeq = std::max(startSeq, log->getCommittedCheckpoint().seq + 1);
    for (uint32_t seq = startSeq; seq < log->getNextSeq(); seq++) {
        const LogEntry &entry = log->getEntry(seq);
        RequestId key = {entry.client_id, entry.client_seq};

        if (!keptReqs.contains(key)) {
            ClientRequest req;
            req.clientId = entry.client_id;
            req.clientSeq = entry.client_seq;
            req.requestData = entry.request;
            req.deadline = entry.deadline;

            ret.push_back(req);
        }
    }
    return ret;
}

void applySuffix(LogSuffix &logSuffix, std::map<RequestId, std::string> &availableReqs, std::shared_ptr<Log> log)
{
    // This should only be called when current checkpoint is consistent with repair checkpoint
    LOG(INFO) << "logSuffix.checkpoint.seq=" << logSuffix.checkpoint->seq()
              << " logSuffix.checkpoint.digest=" << digest_to_hex(logSuffix.checkpoint->log_digest())
              << " self.checkpoint.seq=" << log->getCommittedCheckpoint().seq
              << " self.checkpoint.digest=" << digest_to_hex(log->getCommittedCheckpoint().logDigest);

    assert(
        logSuffix.checkpoint->seq() <= log->getCommittedCheckpoint().seq ||
        log->getDigest(logSuffix.checkpoint->seq()) == logSuffix.checkpoint->log_digest()

    );

    // If checkpoint is too far ahead, just don't do anything
    if (log->getCommittedCheckpoint().seq >= logSuffix.checkpoint->seq() + logSuffix.entries.size()) {
        LOG(INFO) << "Checkpoint seq=" << log->getCommittedCheckpoint().seq
                  << " is ahead of repair checkpoint seq=" << logSuffix.checkpoint->seq()
                  << " + entries size=" << logSuffix.entries.size() << " so not applying suffix";
        return;
    }

    // First sequence to apply is right after checkpoint
    uint32_t seq = logSuffix.checkpoint->seq() + 1;
    uint32_t idx = 0;

    // Reset the client record to the one in the suffix checkpoint so we can rebuild it
    log->getClientRecord() = logSuffix.checkpoint->client_record();

    LOG(INFO) << "Start applySuffixAfterCheckpoint";
    // 2 Skip the entries that are already in the log (consistent)
    for (; idx < logSuffix.entries.size() && seq < log->getNextSeq(); idx++) {
        const dombft::proto::LogEntry *entry = logSuffix.entries[idx];

        if (seq <= log->getCommittedCheckpoint().seq) {
            log->getClientRecord().update(entry->client_id(), entry->client_seq());
            seq++;
            continue;
        }

        // 2.2 If inconsistency is detected, abort own entries after the last consistent entry
        if (log->getDigest(seq) != entry->digest()) {
            break;
        }

        VLOG(6) << "Skipping c_id=" << entry->client_id() << " c_seq=" << entry->client_seq()
                << " since already in log at seq=" << seq;

        log->getClientRecord().update(entry->client_id(), entry->client_seq());
        seq++;
    }

    // Save any requests that will get aborted
    for (uint32_t i = seq; i < log->getNextSeq(); i++) {
        const LogEntry &entry = log->getEntry(i);
        RequestId key = {entry.client_id, entry.client_seq};
        availableReqs[key] = entry.request;
    }

    LOG(INFO) << "Aborting own entries from seq=" << seq;

    log->abort(seq);

    // Step3. Apply entries after inconsistency is detected or suffix is longer than own log
    for (; idx < logSuffix.entries.size(); idx++) {
        VLOG(2) << "Applying entry at seq=" << seq << " log next seq=" << log->getNextSeq();

        assert(seq == log->getNextSeq());
        const dombft::proto::LogEntry *entry = logSuffix.entries[idx];
        uint32_t clientId = entry->client_id();
        uint32_t clientSeq = entry->client_seq();

        // Get request and check the digest
        RequestId key = {clientId, clientSeq};
        if (!availableReqs.contains(key)) {
            throw std::runtime_error("Missing request in repair proposal!");
            LOG(ERROR) << "Missing request at seq=" << seq << " c_id=" << clientId << " c_seq=" << clientSeq;
        }

        CryptoPP::SHA256 hash;
        std::string digestMyReq;
        CryptoPP::StringSource ss(
            availableReqs[key], true, new CryptoPP::HashFilter(hash, new CryptoPP::StringSink(digestMyReq))
        );

        if (digestMyReq != entry->request_digest()) {
            LOG(ERROR) << "Digest mismatch for entry at seq=" << seq << " c_id=" << clientId << " c_seq=" << clientSeq
                       << " digest=" << digestMyReq << " repair digest=" << entry->request_digest();
            throw std::runtime_error("Request in repair proposal does not match!");
        }

        std::string result;
        if (!log->addEntry(entry->client_id(), clientSeq, entry->request(), result)) {
            // This should not happen!
            VLOG(2) << "Failure to add request in slow path! " << " seq=" << seq << " round=" << logSuffix.round
                    << " client_id=" << clientId << " client_seq=" << entry->client_seq();
            continue;
        }

        VLOG(2) << "PERF event=repair_execute replica_id=" << logSuffix.replicaId << " seq=" << seq
                << " round=" << logSuffix.round << " client_id=" << clientId << " client_seq=" << entry->client_seq()
                << " digest=" << digest_to_hex(log->getDigest());
        seq++;
    }
}
