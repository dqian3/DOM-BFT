#include "lib/common.h"
#include "ooo_bench_service_msg.h"
struct OOOBenchEntry {
    OOOBenchRequest req_;
    OOOBenchReply *rep_;
    rrr::DeferredReply *hdl_;
};

class OOOBenchReplica {
private:
    std::vector<ConcurrentQueue<OOOBenchEntry *>> qu_;
    std::atomic<bool> running_;
    std::vector<std::thread *> replyTds_;
    int replyTdNum_;

public:
    OOOBenchReplica(int replyTdNum = 1);
    ~OOOBenchReplica();
    void OnOOOBenchRequest(const OOOBenchRequest &req, OOOBenchReply *, rrr::DeferredReply *defer);
    void ReplyTd(int id);
    void Run();
    void Stop();
};
