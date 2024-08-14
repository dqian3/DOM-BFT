#include "processes/process_config.h"
#include "proxy.h"

#include <memory>

DEFINE_string(config, "configs/config.yaml", "The config file for the proxy");
DEFINE_uint32(proxyId, 0, "The proxy id.");
// for reorder analysis
DEFINE_bool(proxySimmedCliReq, false, "Whether to simulate client requests.");
DEFINE_uint32(simmedCliNum, 5, "The number of simulated client.");
DEFINE_uint32(proxySimmedCliReqFreq, 200, "The frequency of simulated client requests per sec.");
DEFINE_uint32(proxySimmedCliReqDuration, 5, "The duration of simulated client requests in seconds.");

std::unique_ptr<dombft::Proxy> proxy;
void terminate(int para) { proxy->terminate(); }

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    signal(SIGINT, terminate);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);
    VLOG(1) <<"TEST proxySimmedCliReq"<<FLAGS_proxySimmedCliReq;
    if (FLAGS_proxySimmedCliReq){
        proxy = std::make_unique<dombft::Proxy>(config, FLAGS_proxyId, FLAGS_simmedCliNum, FLAGS_proxySimmedCliReqFreq, FLAGS_proxySimmedCliReqDuration);
    }else{
        proxy = std::make_unique<dombft::Proxy>(config, FLAGS_proxyId);
    }
    proxy->run();
}
