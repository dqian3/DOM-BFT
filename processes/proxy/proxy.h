// C++ Standard Libs
#include <fstream>
#include <queue>
#include <random>
#include <thread>

// Third party libs
#include <yaml-cpp/yaml.h>

// Own libraries
#include "lib/common.h"
#include "lib/crypto/sig_provider.h"
#include "lib/transport/endpoint.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "lib/utils.h"

#include "proto/dombft_proto.pb.h"

#include "lib/config/config_util.h"
#include "lib/config/config_manager.h"
#include "owd_calc.h"

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

    /** Flag to Run/Terminate threads */
    std::atomic<bool> running_;

    SignatureProvider sigProvider_;

    std::unique_ptr<Endpoint> endpoint_;

    /** CalculateLatencyBoundTd updates latencyBound_ and concurrently
     * ForwardRequestsTds read it and included in request messages */
    std::atomic<uint32_t> latencyBound_;

    uint32_t proxyId_;
    uint32_t maxOWD_;
    uint64_t lastDeadline_;
    uint32_t numForwarded_ = 0;
    float offsetCoefficient_;

    int numReceivers_;
    std::vector<Address> receiverAddrs_;

    // Batching
    bool isFirstReq = true;   // client starts open loop after the 1st req is commited
    bool proxyBatchEnabled_;
    uint32_t proxyBatchMaxCount_;
    uint32_t proxyBatchMaxDelay_;
    uint64_t curBatchDelay_ = 0;
    std::vector<dombft::proto::DOMRequest> domReqBatchBuffer_;

public:
    /** Proxy accepts a config file, which contains all the necessary information
     * to instantiate the object, then it can call Run method
     *  */
    Proxy(uint32_t proxyId_);
    ~Proxy();

    void Run();
    void Terminate();

private:
    void
    setDOMRequest(const dombft::proto::ClientRequest &inReq, dombft::proto::DOMRequest &outReq, MessageHeader *hdr);

    void sendReq(uint32_t seq);
};

}   // namespace dombft