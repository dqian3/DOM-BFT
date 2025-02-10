#ifndef LOG_H
#define LOG_H

#include "common.h"
#include "proto/dombft_proto.pb.h"

#include <openssl/sha.h>

#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "lib/application.h"
#include "lib/apps/counter.h"
#include "lib/client_record.h"

#include "lib/log_checkpoint.h"
#include "lib/log_entry.h"

class Log {

private:
    std::deque<LogEntry> log;
    uint32_t nextSeq_;
    LogCheckpoint stableCheckpoint_;

    // The log also keeps track of client records, and will de-deduplicate requests
    ClientRecord clientRecord;

    // The log shares ownership of the application with the replica
    std::shared_ptr<Application> app_;

    std::optional<dombft::proto::Cert> latestCert_;
    uint32_t latestCertSeq_ = 0;

public:
    Log(std::shared_ptr<Application> app);

    bool inRange(uint32_t seq) const;

    // Adds an entry and returns whether it is successful.
    bool addEntry(uint32_t c_id, uint32_t c_seq, const std::string &req, std::string &res);
    bool addCert(uint32_t seq, const dombft::proto::Cert &cert);

    // Abort all requests up to and including seq, as well as app state
    void abort(uint32_t seq);

    // Given a sequence number, commit the request and remove previous state, and save new checkpoint
    void setStableCheckpoint(const LogCheckpoint &checkpoint);
    // Given a snapshot of the state we want to try and match, change our checkpoint to match and reapply our logs
    void applySnapshot(uint32_t seq /*commit messages and signatures*/);

    uint32_t getNextSeq() const;
    const std::string &getDigest() const;
    const std::string &getDigest(uint32_t seq) const;
    const LogEntry &getEntry(uint32_t seq);
    LogCheckpoint &getStableCheckpoint();

    ClientRecord &getClientRecord();

    // Get uncommitted suffix of the loh
    void toProto(dombft::proto::FallbackStart &msg);

    friend std::ostream &operator<<(std::ostream &out, const Log &l);
};

std::ostream &operator<<(std::ostream &out, const Log &l);

#endif