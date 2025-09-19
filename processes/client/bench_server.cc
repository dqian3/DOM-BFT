
// clang-format off
#include <gflags/gflags.h>
#include <glog/logging.h>
#include "ooo_bench_service_implementation.h"
// clang-format on

DEFINE_string(serverName, "localhost", "The serverName");
DEFINE_int32(ioThreads, 1, "The number of IO(epoll) threads used by server");
DEFINE_int32(workerNum, 1, "The number of worker threads");
DEFINE_string(serverAddr, "127.0.0.1", "The addr of this server");
DEFINE_string(serverPort, "19832", "The port of this server");
DEFINE_int32(replyTdNum, 1, "The number of reply threads in Replica");
using namespace rrr;
using namespace OOO_BFT_RPC;
bool should_stop = false;
pthread_mutex_t g_stop_mutex;
pthread_cond_t g_stop_cond;
static void signal_handler(int sig)
{
    Log_info("caught signal %d, stopping server now", sig);
    should_stop = true;
    Pthread_mutex_lock(&g_stop_mutex);
    Pthread_cond_signal(&g_stop_cond);
    Pthread_mutex_unlock(&g_stop_mutex);
}

int main(int argc, char **argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    LOG(INFO) << "Start replyTdNum=" << FLAGS_replyTdNum;
    OOOBenchReplica *replica = new OOOBenchReplica(FLAGS_replyTdNum);

    // Handle client requests
    PollMgr *poll = new PollMgr(FLAGS_ioThreads);
    ThreadPool *thrpool = new ThreadPool(FLAGS_workerNum);
    rrr::Server *svr = new rrr::Server(poll, thrpool);
    OOOBFTBenchServiceImpl *svc = new OOOBFTBenchServiceImpl(replica);
    svr->reg(svc);
    LOG(INFO) << "Start Service serverAddr=" << FLAGS_serverAddr << "--port=" << FLAGS_serverPort;
    svr->start((FLAGS_serverAddr + ":" + FLAGS_serverPort).c_str());

    LOG(INFO) << "Run Replias";
    replica->Run();

    Pthread_mutex_init(&g_stop_mutex, nullptr);
    Pthread_cond_init(&g_stop_cond, nullptr);

    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    signal(SIGALRM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGQUIT, signal_handler);
    signal(SIGTERM, signal_handler);

    Pthread_mutex_lock(&g_stop_mutex);
    while (should_stop == false) {
        Pthread_cond_wait(&g_stop_cond, &g_stop_mutex);
    }
    Pthread_mutex_unlock(&g_stop_mutex);

    svc->StopReplicaProcessingThreads();
    LOG(INFO) << "Server To Stop";
    poll->release();
    thrpool->release();
    LOG(INFO) << "thrpool released";
    delete svr;
    LOG(INFO) << "svr deleted";

    return 0;
}
