
// clang-format off
#include <gflags/gflags.h>
#include <glog/logging.h>
#include "lib/common.h"
#include "ooo_bench_service_implementation.h"
// clang-format on

DEFINE_string(serverName, "localhost", "The serverName");
DEFINE_int32(ioThreads, 1, "The number of IO(epoll) threads used by server");
DEFINE_int32(workerNum, 1, "The number of worker threads");
DEFINE_string(receiver, "127.0.0.1", "The addr of this server");
DEFINE_string(receiverPort, "19832", "The port of this server");
DEFINE_int32(sleepIntervalUs, 1000, "The interval between sending reqs");

using namespace rrr;
using namespace OOO_BFT_RPC;
std::atomic<uint32_t> replyNum;
std::vector<OOOBFTBenchProxy *> proxies;
std::vector<std::thread *> tds;

// Get Current Microsecond Timestamp
int64_t GetMicrosecondTimestamp()
{
    auto tse = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(tse).count();
}

void Send(int id)
{
    rrr::FutureAttr fuattr;
    std::function<void(Future *)> cb = [](Future *fu) {
        OOOBenchReply rep;
        fu->get_reply() >> rep;
        replyNum.fetch_add(1);
    };
    fuattr.callback = cb;
    OOOBenchRequest req;
    req.clientId_ = id;
    LOG(INFO) << "id=" << req.clientId_;
    for (uint32_t i = 0; i < 1000000ul; i++) {
        req.reqId_ = i;
        Future::safe_release(proxies[id]->async_SendOOOBenchRequest(req, fuattr));
        // sleep(1);
        // LOG(INFO) << "id=" << id << "\ti=" << i << "\treplyNum=" << replyNum;
        if (FLAGS_sleepIntervalUs > 0) {
            usleep(FLAGS_sleepIntervalUs);
        }
    }
}
int main(int argc, char **argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    replyNum = 0;
    LOG(INFO) << "workerNum=" << FLAGS_workerNum;
    proxies.resize(FLAGS_workerNum, NULL);
    for (uint32_t i = 0; i < FLAGS_workerNum; i++) {
        PollMgr *rpcPoll = new PollMgr(1);
        Client *cli = new rrr::Client(rpcPoll);
        int ret = 0;
        do {
            ret = cli->connect((FLAGS_receiver + ":" + FLAGS_receiverPort).c_str());
            if (ret != 0) {
                sleep(1);
            }
            LOG(INFO) << "Try Connecting " << i;
        } while (ret != 0);
        OOOBFTBenchProxy *proxy = new OOOBFTBenchProxy(cli);
        proxies[i] = proxy;
    }
    LOG(INFO) << "Lanch Threads";
    tds.resize(FLAGS_workerNum, NULL);

    for (uint32_t i = 0; i < FLAGS_workerNum; i++) {
        tds[i] = new std::thread(Send, i);
    }

    uint64_t lastReplyNum = 0;
    int64_t lastTime = GetMicrosecondTimestamp();
    while (true) {
        sleep(1);
        int64_t currentTime = GetMicrosecondTimestamp();
        uint64_t currentReplNum = replyNum;
        float tp = (currentReplNum - lastReplyNum) * 1000.0 * 1000.0 / (currentTime - lastTime);
        LOG(INFO) << "tp=" << tp << " reqs/sec  " << "replyNum=" << replyNum;
        lastReplyNum = currentReplNum;
        lastTime = currentTime;
    }

    for (uint32_t i = 0; i < FLAGS_workerNum; i++) {
        tds[i]->join();
    }
}