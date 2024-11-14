#include "processes/process_config.h"

#include "lib/common.h"
#include "lib/signature_provider.h"
#include "lib/threadpool.h"
#include "lib/transport/address.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <thread>

#include <yaml-cpp/yaml.h>

enum DummyProtocol { PBFT = 0, ZYZ = 1, DOM_BFT = 2 };

namespace dombft {
class DummyReplica {
private:
    // Replica static config
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;
    std::vector<Address> clientAddrs_;
    uint32_t f_;
    uint32_t numVerifyThreads_;

    DummyProtocol prot_;

    // Helper classes for signatures and threading
    SignatureProvider sigProvider_;

    // Control flow/endpoint objects
    ConcurrentQueue<std::vector<byte>> verifyQueue_;
    ConcurrentQueue<std::vector<byte>> processQueue_;
    ThreadPool sendThreadpool_;

    bool running_;
    std::vector<std::thread> verifyThreads_;
    std::thread processThread_;

    std::unique_ptr<Endpoint> endpoint_;

    // Replica state
    uint32_t nextSeq_ = 1;
    uint32_t committedSeq_ = 0;

    std::map<uint32_t, uint32_t> prepareCounts;
    std::map<uint32_t, uint32_t> commitCounts;

    // TODO use a log here
    // std::shared_ptr<Log> log_;
    // ClientRecords clientRecords_;

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void verifyMessagesThd();
    void processMessagesThd();

    template <typename T> void sendMsgToDst(const T &msg, MessageType type, const Address &dst);
    template <typename T> void broadcastToReplicas(const T &msg, MessageType type);

public:
    DummyReplica(const ProcessConfig &config, uint32_t replicaId, DummyProtocol prot);
    ~DummyReplica();

    void run();
};

}   // namespace dombft