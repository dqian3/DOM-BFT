#include "client.h"

DEFINE_string(config, "configs/config.yaml", "The config file for the client");
DEFINE_uint32(client_id, 0, "The client id.");

dombft::Client* client = NULL;
int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);

    LOG(INFO) << "Loading config information from " << FLAGS_config;

    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    client = new dombft::Client(config, FLAGS_client_id);
    client->Run();
    delete client;
}