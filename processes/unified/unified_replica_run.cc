#include "processes/process_config.h"

#include "unified_replica.h"

DEFINE_string(config, "configs/config.yaml", "The config file for the unified replica");

DEFINE_int32(replicaId, 0, "replica id");
DEFINE_int32(receiverId, -1, "receiver id (defaults to same as replicaId if not specified)");

// Replica-specific flags
DEFINE_bool(crashed, false, "If true, replica will receive messages but not send any messages");
DEFINE_int32(swapFreq, 0, "Trigger recovery or slow path with swap every <swapFreq> requests");
DEFINE_int32(viewChangeFreq, 0, "Trigger one view change every <viewChangeFreq> repairs");
DEFINE_int32(viewChangeNum, 0, "Max number of view changes to trigger");
DEFINE_int32(checkpointDropFreq, 0, "Trigger checkpoint drop every <checkpointDropFreq> checkpoints");
DEFINE_bool(
    commitLocalInViewChange, false,
    "Send pbft commit to self so that it can advance to next round while others are still in the previous round; "
    "work with viewChangeFreq"
);

// Receiver-specific flags
DEFINE_bool(skipForwarding, false, "Whether to skip forwarding (for reordering experiments).");
DEFINE_bool(ignoreDeadlines, false, "Whether to ignore deadlines (for reordering experiments).");

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    // Use replicaId as receiverId if not specified
    uint32_t receiverId = FLAGS_receiverId >= 0 ? FLAGS_receiverId : FLAGS_replicaId;

    LOG(INFO) << "Starting unified replica with replicaId=" << FLAGS_replicaId << " receiverId=" << receiverId;

    dombft::UnifiedReplica unifiedReplica(
        config, FLAGS_replicaId, receiverId, FLAGS_crashed, FLAGS_swapFreq, FLAGS_viewChangeFreq,
        FLAGS_commitLocalInViewChange, FLAGS_viewChangeNum, FLAGS_checkpointDropFreq, FLAGS_skipForwarding,
        FLAGS_ignoreDeadlines
    );

    unifiedReplica.run();

    LOG(INFO) << "Unified replica exited cleanly";
    return 0;
}