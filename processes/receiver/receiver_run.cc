#include "processes/process_config.h"
#include "receiver.h"

DEFINE_string(config, "configs/config.yaml", "The config file for the receiver");

dombft::Receiver *receiver = NULL;
DEFINE_uint32(receiverId, 0, "The receiver id.");
int main(int argc, char *argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    LOG(INFO) << "Loading config from " << FLAGS_config;
    ProcessConfig config;
    config.parseConfig(FLAGS_config);

    dombft::Receiver receiver(config, FLAGS_receiverId);
    receiver.run();
}
