#include "processes/flutter/flutter_replica.h"

#include "lib/config/config_manager.h"
#include "lib/config/process_config.h"

#include <yaml-cpp/yaml.h>

DEFINE_string(config, "configs/replica.yaml", "The config file for the replica");
DEFINE_int32(replicaId, 0, "replica id");
DEFINE_int64(clockBroadcastInterval, 0, "Clock broadcast interval in microseconds (0 = read from config)");

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);
    dombft::ConfigManager::initialize(config);

    // Parse clockBroadcastInterval from YAML if not provided via CLI
    uint64_t clockBroadcastInterval = FLAGS_clockBroadcastInterval;
    if (clockBroadcastInterval == 0) {
        try {
            YAML::Node yamlConfig = YAML::LoadFile(FLAGS_config);
            if (yamlConfig["replica"] && yamlConfig["replica"]["clockBroadcastInterval"]) {
                clockBroadcastInterval = yamlConfig["replica"]["clockBroadcastInterval"].as<uint64_t>();
            } else {
                // Default to 50ms (50000 microseconds) if not specified
                clockBroadcastInterval = 50000;
            }
        } catch (const std::exception &e) {
            LOG(WARNING) << "Failed to parse clockBroadcastInterval from config: " << e.what()
                         << ". Using default 50000us";
            clockBroadcastInterval = 50000;
        }
    }
    LOG(INFO) << "Using clockBroadcastInterval=" << clockBroadcastInterval << "us";

    dombft::FlutterReplica replica(FLAGS_replicaId, clockBroadcastInterval);
    replica.run();

    LOG(INFO) << "Flutter replica exited cleanly";
    return 0;
}