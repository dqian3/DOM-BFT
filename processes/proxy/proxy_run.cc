#include "proxy.h"
#include "processes/process_config.h"

#include <memory>

DEFINE_string(config, "configs/config.yaml", "The config file for the proxy");
DEFINE_uint32(proxy_id, 0, "The proxy id.");

std::unique_ptr<dombft::Proxy> proxy;
void terminate(int para) {
    proxy->terminate();
}

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    signal(SIGINT, terminate);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    proxy = std::make_unique<dombft::Proxy>(config, FLAGS_proxy_id);
    proxy->run();
}
