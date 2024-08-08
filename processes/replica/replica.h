#include "processes/process_config.h"

#include "lib/transport/address.h"
#include "lib/log.h"
#include "lib/message_type.h"
#include "lib/protocol_config.h"
#include "lib/signature_provider.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <queue>
#include <span>

#include <yaml-cpp/yaml.h>

namespace dombft
{
    class Replica
    {
    private:
        uint32_t replicaId_;
        std::vector<Address> replicaAddrs_;
        std::vector<Address> clientAddrs_;

        uint32_t f_;
        uint32_t instance_ = 0;


#if PROTOCOL == PBFT
        std::map<std::pair<int, int>, int> prepareCount;
        std::map<std::pair<int, int>, int> commitCount;
#endif

        /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
        SignatureProvider sigProvider_;

        std::unique_ptr<Endpoint> endpoint_;
        std::unique_ptr<Timer> fallbackStartTimer_;
        std::unique_ptr<Timer> fallbackTimer_;
        std::unique_ptr<Log> log_;        

        // State for commit/checkpoint protocol
        // TODO move this somewhere else?
        std::map<int, dombft::proto::Reply> commitCertReplies;
        std::map<int, std::string> commitCertSigs;

        // State for fallback
        bool fallback_ = false;
        uint32_t fallbackTriggerSeq_ = 0;
        std::map<int, dombft::proto::FallbackStart> fallbackHistory;
        std::map<int, std::string> fallbackHistorySigs;

        std::queue<dombft::proto::ClientRequest> fallbackQueuedReqs_;

        void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
        void handleClientRequest(const dombft::proto::ClientRequest &request);
        void handleCert(const dombft::proto::Cert &cert);
        void handleReply(const dombft::proto::Reply &reply, std::span<byte> sig);
        void handleCommit(const dombft::proto::Commit &commitMsg, std::span<byte> sig);

        void broadcastToReplicas(const google::protobuf::Message &msg, MessageType type);
        bool verifyCert(const dombft::proto::Cert &cert);

        void startFallback();

        void handleFallbackStart(const dombft::proto::FallbackStart &msg, std::span<byte> sig);
        void applyFallbackReq(const dombft::proto::LogEntry &entry);

        void finishFallback(const dombft::proto::FallbackProposal &history);

    public:
        Replica(const ProcessConfig &config, uint32_t replicaId);
        ~Replica();
        
        void run();

    };

} // namespace dombft