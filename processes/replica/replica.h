#include "replica_config.h"

#include "lib/utils.h"
#include "lib/address.h"
#include "proto/dombft_proto.pb.h"
#include "lib/signed_udp_endpoint.h"
#include "lib/ipc_endpoint.h"
#include "lib/common_type.h"

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

        /** The replica uses this endpoint to receive requests from receivers and reply to clients*/
        SignedUDPEndpoint *endpoint_;


    };

} // namespace dombft