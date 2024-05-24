#include "replica.h"
#include "processes/process_config.h"

DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");
DEFINE_int32(replicaId, 0, "replica id");

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    dombft::Replica replica(config, FLAGS_replicaId);
    replica.run();
}
