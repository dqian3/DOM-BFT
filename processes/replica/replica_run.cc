#include "processes/process_config.h"
#include "replica.h"

DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");
DEFINE_int32(replicaId, 0, "replica id");
DEFINE_int32(swapFreq, 0, "Trigger recovery or slow path with swap");

dombft::Replica *replica_ptr = nullptr;

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    dombft::Replica replica(config, FLAGS_replicaId, FLAGS_swapFreq);

    replica.run();

    LOG(INFO) << "Replica exited cleanly";
}
