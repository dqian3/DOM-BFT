#include "client.h"

DEFINE_string(config, "configs/config.yaml", "The config file for the client");
DEFINE_uint32(clientId, 0, "The client id.");

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    dombft::Client client(config, FLAGS_clientId);
}