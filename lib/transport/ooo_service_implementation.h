
// clang-format off
#include "ooo_service_msg.h"
#include "ooo_service.h"
#include <iostream>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <semaphore.h>
#include <shared_mutex>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
// clang-format on
namespace OOO_BFT_RPC {
using OOOHandler = std::function<void(const OOOPrepareRequest &)>;
class OOOBFTServiceImpl : public OOOBFTService {
protected:
    // To add your own instance to handle requests, e.g.,
    // you can add an instance of replica, and call replica->process() to do the handling by yourself
    // Here, I only keep a handler function to show a simple demo
    OOOHandler handler_;

public:
    OOOBFTServiceImpl(const OOOHandler &h);
    void SendOOOPrepareRequest(const OOOPrepareRequest &req, rrr::DeferredReply *defer) override;
};
}   // namespace OOO_BFT_RPC