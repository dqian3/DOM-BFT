#include "replica.h"
DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");


int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    dombft::Replica replica(FLAGS_config);
    replica.Run();
}
