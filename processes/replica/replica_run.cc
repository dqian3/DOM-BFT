#include "processes/process_config.h"

#include "dummy_replica.h"
#include "replica.h"

DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");

DEFINE_int32(replicaId, 0, "replica id");
DEFINE_int32(swapFreq, 0, "Trigger recovery or slow path with swap every <swapFreq> requests");
// It tries to invoke view change by time out in either prepare or commit phase (one by one)
DEFINE_int32(viewChangeFreq, 0, "Trigger one view change every <viewChangeFreq> fallbacks");
DEFINE_bool(commitLocalInViewChange, false, "Send pbft commit to self so that it can advance to next instance while others are still in the previous instance; work with viewChangeFreq");
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
        dombft::Replica replica(config, FLAGS_replicaId, FLAGS_swapFreq, FLAGS_viewChangeFreq, FLAGS_commitLocalInViewChange);
        replica.run();
    }

    LOG(INFO) << "Replica exited cleanly";
}
