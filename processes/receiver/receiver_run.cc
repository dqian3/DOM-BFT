#include "receiver.h"
DEFINE_string(config, "configs/receiver.yaml", "The config file for the receiver");

dombft::Receiver* receiver = NULL;
DEFINE_uint32(receiverId, 0, "The receiver id.");
int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    receiver = new dombft::Receiver(CONFIG_FILENAME, FLAGS_receiverId);
    receiver->Run();
    delete receiver;
}
