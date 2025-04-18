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

enum DummyProtocol { PBFT = 0, ZYZ = 1, DUMMY_DOM_BFT = 2, DOM_BFT = 3 };

namespace dombft {
class DummyReplica {
private:
    // Replica static config
    uint32_t replicaId_;
    std::vector<Address> replicaAddrs_;
    Address receiverAddr_;
    std::vector<Address> clientAddrs_;
    uint32_t f_;
    uint32_t numVerifyThreads_;
    uint32_t batchSize_;

    DummyProtocol prot_;

    // Helper classes for signatures and threading
    SignatureProvider sigProvider_;

    // Control flow/endpoint objects
    BlockingConcurrentQueue<std::vector<byte>> verifyQueue_;
    BlockingConcurrentQueue<std::vector<byte>> processQueue_;
    ThreadPool sendThreadpool_;

    bool running_;
    std::vector<std::thread> verifyThreads_;
    std::thread processThread_;

    std::unique_ptr<Endpoint> endpoint_;

    // Replica state
    uint32_t nextSeq_;
    uint32_t committedSeq_ = 0;

    std::map<uint32_t, uint32_t> prepareCounts;
    std::map<uint32_t, uint32_t> commitCounts;

    std::deque<std::pair<dombft::proto::ClientRequest, std::string>> batchQueue_;

    // TODO use a log here

    void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
    void verifyMessagesThd();
    void processMessagesThd();
    void processClientRequest(const dombft::proto::ClientRequest &request, std::span<byte> sig);

    template <typename T> void sendMsgToDst(const T &msg, MessageType type, const Address &dst);
    template <typename T> void broadcastToReplicas(const T &msg, MessageType type);

public:
    DummyReplica(const ProcessConfig &config, uint32_t replicaId, DummyProtocol prot, uint32_t batchSize = 1);
    ~DummyReplica();

    void run();
};

}   // namespace dombft