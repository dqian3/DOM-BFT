#include "processes/process_config.h"
#include "proxy.h"

#include <memory>

DEFINE_string(config, "configs/config.yaml", "The config file for the proxy");
DEFINE_uint32(proxyId, 0, "The proxy id.");

// for reordering experiments
DEFINE_bool(genRequests, false, "Whether to simulate client requests.");
DEFINE_uint32(rate, 10, "The rate of simulated client requests per sec.");
DEFINE_uint32(duration, 2, "The duration of simulated client requests in seconds.");
DEFINE_bool(poisson, false, "The duration of simulated client requests in seconds.");

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
    if (FLAGS_genRequests) {
        proxy = std::make_unique<dombft::Proxy>(config, FLAGS_proxyId, FLAGS_rate, FLAGS_duration, FLAGS_poisson);
    } else {
        proxy = std::make_unique<dombft::Proxy>(config, FLAGS_proxyId);
    }
    proxy->run();
}
