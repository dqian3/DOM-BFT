#include "receiver/receiver.h"
DEFINE_string(config, "configs/receiver.yaml", "The config file for the receiver");

dombft::Receiver* receiver = NULL;
void Terminate(int para) {
    receiver->Terminate();
}

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    signal(SIGINT, Terminate);
    receiver = new dombft::Receiver(FLAGS_config);
    receiver->Run();
    delete receiver;
}
