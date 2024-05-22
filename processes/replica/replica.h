#include "replica_config.h"

#include "lib/address.h"
#include "lib/config.h"
#include "lib/ipc_endpoint.h"
#include "lib/log.h"
#include "lib/message_type.h"
#include "lib/signature_provider.h"
#include "lib/udp_endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

#include <yaml-cpp/yaml.h>

namespace dombft
{
    class Replica
    {
    private:
        /** All the configuration parameters for the replica */
        ReplicaConfig replicaConfig_;

        /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
        SignatureProvider sigProvider_;

        std::unique_ptr<UDPEndpoint> endpoint_;
        std::unique_ptr<MessageHandler> handler_;
        std::unique_ptr<Log> log_;

        std::vector<Address> replicaAddrs_;

        uint32_t seq_ = 0;


        void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
        void handleClientRequest(const dombft::proto::ClientRequest &request, uint32_t seq);

        void broadcastToReplicas(const google::protobuf::Message &msg, MessageType type);

#if PROTOCOL == PBFT
        std::map<std::pair<int, int>, int> prepareCount;
        std::map<std::pair<int, int>, int> commitCount;
#endif


    public:

        Replica(const std::string &configFile);
        void run();
        ~Replica();


    };

} // namespace dombft