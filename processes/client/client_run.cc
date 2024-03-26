#include "client.h"
DEFINE_string(config, "configs/client.yaml", "The config file for the client");
dombft::Client* client = NULL;
int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    client = new dombft::Client(FLAGS_config);
    client->Run();
    delete client;
}