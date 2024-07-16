#include "processes/process_config.h"

#include "lib/transport/address.h"
#include "lib/log.h"
#include "lib/message_type.h"
#include "lib/protocol_config.h"
#include "lib/signature_provider.h"
#include "lib/transport/endpoint.h"
#include "lib/utils.h"
// #include "lib/apps/kv_rocksdb.h"
#include "proto/dombft_proto.pb.h"
#include "lib/apps/database.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

#include <mutex>

#include <yaml-cpp/yaml.h>

namespace dombft
{
    class Replica
    {
    private:

        // this is for the state of the replica, whether it can take the fast path now or not. 
        // in the future, it may be extended to more states of the replica
        // when a request comes in, based on the current state of the replica, the replica will decide whether to take the fast path or not
        enum class ReplicaState
        {
            NORMAL_PATH,
            FAST_PATH, 
            SLOW_PATH,
        };

        uint32_t replicaId_;
        std::vector<Address> replicaAddrs_;

        std::vector<Address> clientAddrs_;
        uint32_t clientPort_;

        uint32_t f_;

        // KVStore db_;

        InMemoryDB inMemoryDB_;
        // an interval after which we forced a normal path.
        int forceNormal_;

        // init the atomic variable of the state of the replica. 
        ReplicaState replicaState_;
        std::mutex stateMutex_;

        // init thhe biggest deadline received so far. 
        // if a message with a larger deadline is received, switch to normal path (or slow path? )
        uint64_t biggestDeadline_;
        std::mutex deadlineMutex_;

#if PROTOCOL == PBFT
        std::map<std::pair<int, int>, int> prepareCount;
        std::map<std::pair<int, int>, int> commitCount;
#endif

        /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
        SignatureProvider sigProvider_;

        std::unique_ptr<Endpoint> endpoint_;
        std::unique_ptr<Log> log_;

        void handleMessage(MessageHeader *msgHdr, byte *msgBuffer, Address *sender);
        void handleClientRequest(const dombft::proto::ClientRequest &request);
        void handleCert(const dombft::proto::Cert &cert);

        void broadcastToReplicas(const google::protobuf::Message &msg, MessageType type);
        void setState(ReplicaState newState);
        ReplicaState getState();

        void updateDeadline(uint64_t deadline);

        // check if we need to force a normal path for this request. 
        bool needToForceNormal();

    public:
        Replica(const ProcessConfig &config, uint32_t replicaId);
        ~Replica();
        
        void run();

    };

} // namespace dombft