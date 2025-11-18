#include "flutter_client.h"

#include "lib/config/config_manager.h"
#include "lib/config/process_config.h"

#include <chrono>
#include <thread>

DEFINE_string(config, "configs/config.yaml", "The config file for the client");
DEFINE_uint32(clientId, 0, "The client id");
DEFINE_uint64(baseBetOffset, 100000, "Base bet offset (us) to use for requests");
DEFINE_uint64(betIncrement, 100000, "Amount to increase bet on retry");

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);
    dombft::ConfigManager::initialize(config);

    LOG(INFO) << "Starting Flutter Client " << FLAGS_clientId << " with base bet " << FLAGS_baseBetOffset
              << " and bet offset increment " << FLAGS_betIncrement;

    dombft::FlutterClient client(FLAGS_clientId, FLAGS_baseBetOffset, FLAGS_betIncrement);

    // Start client in background thread
    std::thread clientThread([&client]() { client.run(); });

    LOG(INFO) << "Flutter client exiting";
    client.stop();

    if (clientThread.joinable()) {
        clientThread.join();
    }

    return 0;
}