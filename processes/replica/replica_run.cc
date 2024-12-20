#include "processes/process_config.h"

#include "dummy_replica.h"
#include "replica.h"

DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");

DEFINE_int32(replicaId, 0, "replica id");
DEFINE_int32(swapFreq, 0, "Trigger recovery or slow path with swap");
DEFINE_string(prot, "dombft", "Protocol for the replica");

dombft::Replica *replica_ptr = nullptr;

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    if (FLAGS_prot == "PBFT") {
        dombft::DummyReplica replica(config, FLAGS_replicaId, DummyProtocol::PBFT);
        replica.run();
    } else if (FLAGS_prot == "ZYZ") {
        dombft::DummyReplica replica(config, FLAGS_replicaId, DummyProtocol::ZYZ);
        replica.run();
    } else if (FLAGS_prot == "DUMMY_DOMBFT") {
        dombft::DummyReplica replica(config, FLAGS_replicaId, DummyProtocol::DUMMY_DOM_BFT);
        replica.run();
    } else {
        dombft::Replica replica(config, FLAGS_replicaId, FLAGS_swapFreq);
        replica.run();
    }

    LOG(INFO) << "Replica exited cleanly";
}
