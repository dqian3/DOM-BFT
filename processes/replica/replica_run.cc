#include "replica.h"
DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");

dombft::Receiver* replica = NULL;

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    replica = new dombft::Receiver(FLAGS_config);
    replica->Run();
    delete replica;
}
