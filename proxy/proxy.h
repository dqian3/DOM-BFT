// C++ Standard Libs
#include <fstream>
#include <thread>

// Third party libs
#include <yaml-cpp/yaml.h>

// Own libraries
#include "lib/utils.h"
#include "lib/endpoint.h"
#include "lib/signed_udp_endpoint.h"

#include "proto/dombft_proto.pb.h"

#include "proxy_config.h"

namespace dombft
{

    /**
     * Refer to proxy_run.cc, the runnable program only needs to instantiate a
     * Proxy object with a configuration file. Then it calls Run() method to run
     * and calls Terminate() method to stop
     */

    class Proxy
    {
    private:
        /** All the configuration parameters for this proxy are included in
         * proxyConfig_*/
        ProxyConfig proxyConfig_;
        /** Each thread is given a unique name (key) */
        std::map<std::string, std::thread *> threadPool_;

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

        /** LogTd is just used to collect some performance stats. It is not necessary
         * in the release version */
        void LogTd();

        /** Flag to Run/Terminate threads */
        std::atomic<bool> running_;

        /** Each CheckQuorumTd thread uses a udp socket inside measurementEp_, based on its
         * id, to send reply to clients
         */
        Endpoint *measurmentEp_;

        /** Each ForwardRequestsTd thread uses a udp socket in forwardEps_, based on
         * its id, to multicast requests to replicas
         */
        std::vector<SignedUDPEndpoint *> forwardEps_;

        /** CalculateLatencyBoundTd updates latencyBound_ and concurrently
         * ForwardRequestsTds read it and included in request messages */
        std::atomic<uint32_t> latencyBound_;

        /** Upper bound of the estimated latencyBound_, used to clamp the bound,
         * details in ``Adapative Latency Bound`` para of Sec 4 of our paper */
        uint32_t maxOWD_;

        int numReceivers_;
        std::vector<Address> receiverAddrs_;

        /** Just used to collect logs, can be deleted in the release version*/
        struct Log
        {
            uint32_t replicaId_;
            uint32_t clientId_;
            uint32_t reqId_;
            uint64_t clientTime_;
            uint64_t proxyTime_;
            uint64_t proxyEndProcessTime_;
            uint64_t recvTime_;
            uint64_t deadline_;
            uint64_t fastReplyTime_;
            uint64_t slowReplyTime_;
            uint64_t proxyRecvTime_;
            uint32_t commitType_;

            Log(uint32_t rid = 0, uint32_t cId = 0, uint32_t reqId = 0,
                uint64_t ctime = 0, uint64_t ptime = 0, uint64_t pedtime = 0,
                uint64_t rtime = 0, uint64_t ddl = 0, uint64_t fttime = 0,
                uint64_t swtime = 0, uint64_t prcvt = 0, uint32_t cmtt = 0)
                : replicaId_(rid),
                  clientId_(cId),
                  reqId_(reqId),
                  clientTime_(ctime),
                  proxyTime_(ptime),
                  recvTime_(rtime),
                  deadline_(ddl),
                  fastReplyTime_(fttime),
                  slowReplyTime_(swtime),
                  proxyRecvTime_(prcvt),
                  commitType_(cmtt) {}
            std::string ToString()
            {
                return std::to_string(replicaId_) + "," + std::to_string(clientId_) +
                       "," + std::to_string(reqId_) + "," + std::to_string(clientTime_) +
                       "," + std::to_string(proxyTime_) + "," +
                       std::to_string(proxyEndProcessTime_) + "," +
                       std::to_string(recvTime_) + "," + std::to_string(deadline_) + "," +
                       std::to_string(fastReplyTime_) + "," +
                       std::to_string(slowReplyTime_) + "," +
                       std::to_string(proxyRecvTime_) + "," + std::to_string(commitType_);
            }
        };
        ConcurrentQueue<Log> logQu_;
        std::vector<ConcurrentMap<uint64_t, Log *>> logMap_;

    public:
        /** Proxy accept a config file, which contains all the necessary information
         * to instantiate the object, then it can call Run method
         *  */
        Proxy(const std::string &configFile = "../configs/nezha-proxy-config.yaml");
        ~Proxy();
        void Run();
        void Terminate();

        /** Tentative */
        std::vector<std::vector<uint64_t>> replicaSyncedPoints_;
    };

} // namespace nezha