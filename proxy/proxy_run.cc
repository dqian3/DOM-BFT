#include "proxy/proxy.h"
DEFINE_string(config, "nezhav2/config/nezha-proxy-config-0.yaml", "The config file for the proxy");

dombft::Proxy* proxy = NULL;
void Terminate(int para) {
    proxy->Terminate();
}

int main(int argc, char* argv[]) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    signal(SIGINT, Terminate);
    proxy = new dombft::Proxy(FLAGS_config);
    proxy->Run();
    delete proxy;
}
