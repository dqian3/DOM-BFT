#include "flutter_client.h"

#include "lib/config/config_manager.h"
#include "lib/config/process_config.h"

#include <chrono>
#include <thread>

DEFINE_string(config, "configs/config.yaml", "The config file for the client");
DEFINE_uint32(clientId, 0, "The client id");
DEFINE_uint64(baseBet, 100000, "Base bet offset (us) to use for requests");
DEFINE_uint64(betIncrement, 100000, "Amount to increase bet on retry");
DEFINE_uint32(maxRetries, 5, "Maximum number of retries for rejected requests");
DEFINE_uint32(numRequests, 10, "Number of requests to send");
DEFINE_uint32(requestInterval, 1000, "Interval between requests in milliseconds");

int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config information from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);
    dombft::ConfigManager::initialize(config);

    LOG(INFO) << "Starting Flutter Client " << FLAGS_clientId
              << " with base bet " << FLAGS_baseBet
              << " and bet increment " << FLAGS_betIncrement;

    dombft::FlutterClient client(FLAGS_clientId, FLAGS_baseBet, FLAGS_betIncrement, FLAGS_maxRetries);

    // Start client in background thread
    std::thread clientThread([&client]() {
        client.run();
    });

    // Submit requests
    for (uint32_t i = 0; i < FLAGS_numRequests; i++) {
        std::string requestData = "Flutter request " + std::to_string(i) + " from client " + std::to_string(FLAGS_clientId);

        LOG(INFO) << "Submitting request " << i << ": " << requestData;
        client.submitRequest(requestData);

        if (i < FLAGS_numRequests - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(FLAGS_requestInterval));
        }
    }

    // Wait a bit for responses
    LOG(INFO) << "All requests submitted. Waiting for responses...";
    std::this_thread::sleep_for(std::chrono::seconds(10));

    LOG(INFO) << "Flutter client exiting";
    client.stop();

    if (clientThread.joinable()) {
        clientThread.join();
    }

    return 0;
}