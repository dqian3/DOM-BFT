#include "client_config.h"

#include <fstream>
#include <iostream>
#include <thread>

#include "lib/utils.h"
#include "lib/address.h"
#include "proto/dombft_proto.pb.h"
#include "lib/udp_endpoint.h"
#include "lib/signature_provider.h"
#include "lib/message_type.h"

#include <yaml-cpp/yaml.h>

namespace dombft
{
    /** LogInfo is used to dump some performance stats, which can be extended to
     * include more metrics */
    struct LogInfo
    {
        uint32_t reqId;
        uint64_t sendTime;
        uint64_t commitTime;
        uint32_t commitType;
        std::string toString()
        {
            std::string ret =
                (std::to_string(reqId) + "," + std::to_string(sendTime) + "," +
                 std::to_string(commitTime) + "," + std::to_string(commitType));
            return ret;
        }
    };
    class Client
    {
    private:
        /** All the configuration parameters for client are included in
         * clientConfig_*/
        ClientConfig clientConfig_;

        /** The endpoint uses to submit request to proxies and receive replies*/
        UDPEndpoint *endpoint_;

        SignatureProvider sigProvider_;

        /** The message handler used to handle replies (from replicas) */
        struct MessageHandler *replyHandler_;

        /** The addresses of proxies. */
        std::vector<Address> proxyAddrs_;


        /** Each client is assigned with a unqiue id */
        int clientId_;

        /** The next requestId to be submitted */
        uint32_t nextReqSeq_ = 0;

        /* State for the currently pending request */
        uint64_t sendTime_;

        // Counters for the number of replies 
        uint32_t numReplies_ = 0;
        uint32_t numFastReplies_ = 0;
        uint32_t numExecuted_ = 0;

        /** The message handler to handle messages */
        void ReceiveReply(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
        void SubmitRequest();


    public:
        /** Client accepts a config file, which contains all the necessary information
         * to instantiate the object, then it can call Run method
         *  */
        Client(const std::string &configFile = "../configs/nezha-client-config.yaml");
        void Run();
        ~Client();

    };

} // namespace nezha