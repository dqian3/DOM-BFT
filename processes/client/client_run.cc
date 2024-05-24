#include "client.h"
DEFINE_string(config, "configs/client.yaml", "The config file for the client");

DEFINE_uint32(client_id, 0, "The client id.");
dombft::Client* client = NULL;
int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;

    ClientConfig config;
    std::string error = config.parseUnifiedConfig(FLAGS_config, FLAGS_config);
    if (error != "")
    {
        LOG(ERROR) << "Error loading client config: " << error << " Exiting.";
        exit(1);
    }


    client = new dombft::Client(FLAGS_client_id);
    client->Run();
    delete client;
}