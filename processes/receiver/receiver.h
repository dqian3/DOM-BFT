#include "processes/process_config.h"

#include "lib/protocol_config.h"
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
    class Receiver
    {
    private:
        SignatureProvider sigProvider_;

        /** The receiver uses this endpoint to receive requests from proxies and reply with OWD measurements*/
        std::unique_ptr<UDPEndpoint> endpoint_;

        /** The receiver optionally uses this endpoint to forward messages along IPC to the receiver*/
        // TODO this could probably be handled in a cleaner way
        // std::unique_ptr<IPCEndpoint> ipc_endpoint;

        /** The handler objects for our endpoint library */
        std::unique_ptr<MessageHandler>  msgHandler_;
        std::unique_ptr<Timer>  fwdTimer_;

        // TODO storing these protobuf objects like this might not be great performance wise
        // couldn't find much about this.
        std::map<std::pair<uint64_t, uint32_t>, dombft::proto::DOMRequest> deadlineQueue_;
        std::vector<dombft::proto::DOMRequest> lateMessages;


        /** The actual message / timeout handlers */
        void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

        void forwardRequest(const dombft::proto::DOMRequest &request);
        void checkDeadlines();


        uint32_t receiverId_;
        uint32_t proxyMeasurementPort_;
        std::vector<Address> replicaAddrs_;


    public:
        Receiver(const ProcessConfig &config, uint32_t receiverId);
        ~Receiver();
        void run();

    };

} // namespace dombft