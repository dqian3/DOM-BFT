#include "receiver_config.h"

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
    class Receiver
    {
    private:
        /** All the configuration parameters for the receiver */
        ReceiverConfig receiverConfig_;

        SignatureProvider sigProvider_;

        /** The receiver uses this endpoint to receive requests from proxies and reply with OWD measurements*/
        UDPEndpoint *endpoint_;

        /** The receiver optionally uses this endpoint to forward messages along IPC to the receiver*/
        // TODO this could probably be handled in a cleaner way
        IPCEndpoint *ipc_endpoint;

        /** Or it sends messages to addresses in this vector */
        std::vector<Address> replicaAddrs_;

        // TODO storing these protobuf objects like this might not be great performance wise
        // couldn't find much about this.
        std::map<std::pair<uint64_t, uint32_t>, dombft::proto::DOMRequest> deadlineQueue_;
        std::vector<dombft::proto::DOMRequest> lateMessages;

        /** The handler objects for our endpoint library */
        MessageHandler *msgHandler_;
        Timer *fwdTimer_;

        /** The actual message / timeout handlers */
        void receiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);

        void forwardRequest(const dombft::proto::DOMRequest &request);
        void checkDeadlines();


    public:
        /** Receiver accepts a config file, which contains all the necessary information
         * to instantiate the object, then it can call Run method
         *  */
        Receiver(const std::string &configFile);
        Receiver(const std::string &configFile, const uint32_t receiverId);
        void Run();
        ~Receiver();

    };

} // namespace dombft