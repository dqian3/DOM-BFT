#include "lib/config/config_manager.h"
#include "lib/config/process_config.h"
#include "proxy.h"

#include <memory>

DEFINE_string(config, "configs/config.yaml", "The config file for the experiment");
DEFINE_uint32(proxyId, 0, "The proxy id.");

std::unique_ptr<dombft::Proxy> proxy;

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);
    dombft::ConfigManager::initialize(config);
    proxy = std::make_unique<dombft::Proxy>(FLAGS_proxyId);
    proxy->Run();
}
