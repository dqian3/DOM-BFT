// C++ Standard Libs
#include <fstream>
#include <queue>
#include <random>
#include <thread>

// Third party libs
#include <openssl/pem.h>
#include <yaml-cpp/yaml.h>

// Own libraries
#include "lib/message_type.h"
#include "lib/protocol_config.h"
#include "lib/signature_provider.h"
#include "lib/transport/endpoint.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "lib/utils.h"
#include "processes/config_util.h"

#include "proto/dombft_proto.pb.h"

#include "owd_calc.h"
#include "processes/process_config.h"
namespace dombft {

/**
 * Refer to proxy_run.cc, the runnable program only needs to instantiate a
 * Proxy object with a configuration file. Then it calls Run() method to run
 * and calls Terminate() method to stop
 */

class Proxy {
private:
    /** Each thread is given a unique name (key) */
    std::map<std::string, std::unique_ptr<std::thread>> threads_;

    /** Launch threads:
     * (1) ForwardRequestsTd, which receives client requests, signs and
     * multicast to replicas;
     * (2) RecvMeasurementsTd, which receives OWD measurements from the receivers
     *
     * (1) handles the most workload and is parallelized, and the parallism
     * degree is decided by the parameter defined in proxyConfig_ (i.e.,
     * shard-num).
     *
     */
    void LaunchThreads();
    void ForwardRequestsTd(const int id = -1);
    void RecvMeasurementsTd();

    void sendReq(uint32_t seq);
    void GenerateRequestsTd();

    /** Flag to Run/Terminate threads */
    std::atomic<bool> running_;

    SignatureProvider sigProvider_;

    std::unique_ptr<Endpoint> measurementEp_;
    std::vector<std::unique_ptr<Endpoint>> forwardEps_;

    /** CalculateLatencyBoundTd updates latencyBound_ and concurrently
     * ForwardRequestsTds read it and included in request messages */
    std::atomic<uint32_t> latencyBound_;

    uint32_t proxyId_;
    uint32_t maxOWD_;
    uint64_t lastDeadline_;
    int numShards_;
    int numReceivers_;
    std::vector<Address> receiverAddrs_;

    // Reordering Experiments
    bool selfGenReqs_;
    uint32_t genReqFreq_;
    uint32_t genReqDuration_;
    bool genReqPoisson_;

public:
    /** Proxy accepts a config file, which contains all the necessary information
     * to instantiate the object, then it can call Run method
     *  */
    Proxy(const ProcessConfig &config, uint32_t proxyId_);

    // Create a proxy that generates requests on its own, for DOM experiments
    Proxy(const ProcessConfig &config, uint32_t proxyId, uint32_t genReqFreq, uint32_t genReqDuration,
          bool genReqPoisson);
    ~Proxy();

    void run();
    void terminate();
};

}   // namespace dombft