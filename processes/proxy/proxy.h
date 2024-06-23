// C++ Standard Libs
#include <fstream>
#include <thread>
#include <queue>

// Third party libs
#include <yaml-cpp/yaml.h>
#include <openssl/pem.h>

// Own libraries
#include "lib/protocol_config.h"
#include "lib/utils.h"
#include "lib/transport/endpoint.h"
#include "lib/signature_provider.h"
#include "lib/message_type.h"
#include "lib/transport/nng_endpoint.h"
#include "lib/transport/udp_endpoint.h"
#include "processes/config_util.h"

#include "proto/dombft_proto.pb.h"

#include "processes/process_config.h"

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
        /** Each thread is given a unique name (key) */
        std::map<std::string, std::thread *> threads_;

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

    public:
        /** Proxy accept a config file, which contains all the necessary information
         * to instantiate the object, then it can call Run method
         *  */
        Proxy(const ProcessConfig &config, uint32_t proxyId_);
        ~Proxy();
        void run();
        void terminate();
    };

    
    namespace OWDCalc{
        // A Strategy pattern
        class BaseCalcStrategy{
        public:
            virtual ~BaseCalcStrategy() = default;
            virtual void addMeasure(uint32_t measure) = 0;
            virtual uint32_t getOWD() const = 0;
        };

        class MeanStrategy : public BaseCalcStrategy {
        public:
            explicit MeanStrategy(uint32_t windowSize){}
            inline void addMeasure(uint32_t measure) override{
                storedMeasure_.push_back(measure);
            }
            inline uint32_t getOWD() const override{
                uint32_t sum = 0;
                for (auto &measure: storedMeasure_) {
                    sum += measure;
                }
                return sum / storedMeasure_.size();
            }
        private:
            std::vector<uint32_t> storedMeasure_;
        };

        class MaxStrategy : public BaseCalcStrategy {
        public:
            inline void addMeasure(uint32_t measure) override{
                storedMeasure_ = std::max(storedMeasure_, measure);
            }
            inline uint32_t getOWD() const override{
                return storedMeasure_;
            }
        private:
            uint32_t storedMeasure_;
        };

        class PercentileStrategy : public BaseCalcStrategy {
        public:
            explicit PercentileStrategy(uint32_t percentile, uint32_t windowSize = 0) :
                windowSize_(windowSize),percentile_(percentile),windowIndex_(0) {storedMeasure_.resize(windowSize);}
            inline void addMeasure(uint32_t measure) override{
                storedMeasure_[windowIndex_] = measure;
                windowIndex_ = windowSize_ ? (windowIndex_ + 1) % windowSize_ : windowIndex_ + 1;
            }
            inline uint32_t getOWD() const override{
                std::vector<uint32_t> sortedMeasure = storedMeasure_;
                std::sort(sortedMeasure.begin(), sortedMeasure.end());
                return sortedMeasure[sortedMeasure.size() * percentile_ / 100];
            }
        private:
            uint32_t windowSize_;
            uint32_t percentile_;
            uint32_t windowIndex_;
            std::vector<uint32_t> storedMeasure_;
        };
        template <typename T>
        class MeasureContext{
        public:
            MeasureContext(uint32_t numReceivers,T strategy,uint32_t windowSize = 0) :
                    numReceivers_(numReceivers), windowSize_(windowSize){
                for (uint32_t i = 0; i < numReceivers_; i++){
                    receiverOWDs_.push_back(std::make_unique<T>(strategy));
                }

            }
            inline void addMeasure(uint32_t rcvrIndex,uint32_t measure){
                receiverOWDs_[rcvrIndex]->addMeasure(measure);
            }
            inline uint32_t getOWD() const{
                // find the largest OWD among all receivers that is smaller than 88
                uint32_t maxOWD = 88;
                return 0;
            }
        private:
            uint32_t numReceivers_;
            uint32_t windowSize_;
            std::vector<std::unique_ptr<BaseCalcStrategy>> receiverOWDs_;
        };

    }

} // namespace nezha