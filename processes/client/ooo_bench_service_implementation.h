
// clang-format off
#include "ooo_bench_service_msg.h"
#include "ooo_bench_service.h"
#include "ooo_bench_replica.h"
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
class OOOBFTBenchServiceImpl : public OOOBFTBenchService {
protected:
    OOOBenchReplica *replica_;

public:
    OOOBFTBenchServiceImpl(OOOBenchReplica *replica);
    void SendOOOBenchRequest(const OOOBenchRequest &req, OOOBenchReply *, rrr::DeferredReply *defer) override;
    void StopReplicaProcessingThreads();
};
}   // namespace OOO_BFT_RPC