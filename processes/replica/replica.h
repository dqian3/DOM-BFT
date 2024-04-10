#include "replica_config.h"

#include "lib/config.h"
#include "lib/utils.h"
#include "lib/address.h"
#include "proto/dombft_proto.pb.h"
#include "lib/udp_endpoint.h"
#include "lib/ipc_endpoint.h"
#include "lib/signature_provider.h"
#include "lib/message_type.h"

#include <fstream>
#include <iostream>
#include <thread>

#include <yaml-cpp/yaml.h>

namespace dombft
{
    class Replica
    {
    private:
        /** All the configuration parameters for the replica */
        ReplicaConfig replicaConfig_;
        SignatureProvider sigProvider_;

        /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
        UDPEndpoint *endpoint_;
        MessageHandler *handler_;

        std::vector<Address> replicaAddrs_;

        uint32_t seq_ = 0;


        void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
        void handleClientRequest(const dombft::proto::ClientRequest &request);


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