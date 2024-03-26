#include "receiver_config.h"

#include "lib/utils.h"
#include "lib/address.h"
#include "proto/dombft_proto.pb.h"
#include "lib/signed_udp_endpoint.h"
#include "lib/ipc_endpoint.h"
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

        /** The receiver uses this endpoint to receive requests from proxies and reply with OWD measurements*/
        SignedUDPEndpoint *endpoint_;

        /** The receiver optionally uses this endpoint to forward messages along IPC to the receiver*/
        // TODO this could probably be handled in a cleaner way
        IPCEndpoint *ipc_endpoint;

        /** Or it sends messages to addresses in this vector */
        std::vector<Address> replicaAddrs_;

        /** The message handler used to handle requests (from proxies) */
        struct MessageHandler *replyHandler_;

        struct std::map<std::pair<uint64_t, uint32_t>, std::vector<unsigned char>> deadlineQueue_;

        /** The message handler to handle messages from proxies. Called from replyHandler_ */
        void ReceiveRequest(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);


    public:
        /** Receiver accepts a config file, which contains all the necessary information
         * to instantiate the object, then it can call Run method
         *  */
        Receiver(const std::string &configFile);
        void Run();
        ~Receiver();

    };

} // namespace dombft