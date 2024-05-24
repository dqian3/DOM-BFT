#include "client_config.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <thread>

#include "lib/address.h"
#include "lib/config.h"
#include "lib/message_type.h"
#include "lib/signature_provider.h"
#include "lib/udp_endpoint.h"
#include "lib/utils.h"
#include "proto/dombft_proto.pb.h"

#include <yaml-cpp/yaml.h>

namespace dombft
{
    struct RequestState
    {
        std::map<int, dombft::proto::Reply> replies;
        std::map<int, std::string> signatures;
        std::optional<dombft::proto::Cert> cert;
        uint64_t sendTime;
        uint64_t certTime;

        std::set<int> certReplies;


        bool fastPathPossible = true;
    };

    class Client
    {
    private:
        /** All the configuration parameters for client are included in
         * clientConfig_*/
        ClientConfig clientConfig_;

        /** The endpoint uses to submit request to proxies and receive replies*/
        std::unique_ptr<UDPEndpoint> endpoint_;
        /** The message handler used to handle replies (from replicas) */
        std::unique_ptr<MessageHandler> replyHandler_;
        /** Timer to handle request timeouts TODO (timeouts vs repeated timer would maybe be better)*/
        std::unique_ptr<Timer> timeoutTimer_;

        SignatureProvider sigProvider_;


        /** The addresses of proxies. */
        std::vector<Address> proxyAddrs_;
        std::vector<Address> replicaAddrs_;
        uint32_t f_;

        /** Each client is assigned with a unqiue id */
        int clientId_;
        uint32_t maxInFlight_ = 0;

        /** The next requestId to be submitted */
        uint32_t nextReqSeq_ = 0;
        uint32_t inFlight_ = 0;
        uint32_t numExecuted_ = 0;


        /* State for the currently pending request */ 
        std::map<int, RequestState> requestStates_;
 
        /** The message handler to handle messages */
        void receiveReply(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
        void checkReqState(uint32_t client_seq);

        void submitRequest();

        void checkTimeouts();


    public:
        /** Client accepts a config file, which contains all the necessary information
         * to instantiate the object, then it can call Run method
         *  */
        Client(const std::string &configFile = "../configs/nezha-client-config.yaml");
        Client(const size_t clientId);
        void Run();
        ~Client();

    };

} // namespace nezha