#include "client.h"
DEFINE_string(config, "configs/client.yaml", "The config file for the client");
//define a new string called clientId and read it from command line 
//use that client id and pass it as a constructor arg.
DEFINE_uint32(client_id, 0, "The client id.");
dombft::Client* client = NULL;
int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    client = new dombft::Client(FLAGS_client_id);
    client->Run();
    delete client;
}