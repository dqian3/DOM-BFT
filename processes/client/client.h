#include "client_config.h"

#include <fstream>
#include <iostream>
#include <thread>

#include "lib/utils.h"
#include "lib/address.h"
#include "proto/dombft_proto.pb.h"
#include "lib/signed_udp_endpoint.h"
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
        SignedUDPEndpoint *endpoint_;

        /** The message handler used to handle replies (from replicas) */
        struct MessageHandler *replyHandler_;


        /** Each client is assigned with a unqiue id */
        int clientId_;

        /** The next requestId to be submitted */
        uint32_t nextReqSeq_;

        /** The addresses of proxies. */
        std::vector<Address> proxyAddrs_;

        /** Counters for the number of replies */
        uint32_t numReplies_;
        uint32_t numFastReplies_;
        uint32_t numExecuted_;

        /** The message handler to handle messages from proxies. The function is used
         * to instantiate a replyHandler_ and registered to requestEP_ */
        void ReceiveReply(MessageHeader *msgHdr, char *msgBuffer, Address *sender);
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