#ifndef OOO_RPC_ENDPOINT_H
#define OOO_RPC_ENDPOINT_H

#include "lib/transport/endpoint.h"
#include "lib/transport/ooo_service_implementation.h"

/** demo handler for test only */
inline void OOORPCMsgHandler(const std::string &msg) { printf("The message is %s", msg.c_str()); }

using namespace OOO_BFT_RPC;

struct OOOProxyInfo {
    rrr::Client *client_;
    OOOBFTProxy *proxy_;
};
class OOORPCEndpoint : public Endpoint {
protected:
    // Maintain a proxy for each dest-node that this endpoint will SendPreparedMsgTo
    std::string myIP_;
    uint32_t myListeningPort_;
    u_int32_t numProxiesPerAddr_;

    MessageHandlerFunc hdlrFunc_;
    OOOHandler oooHdl_;

    rrr::PollMgr *clientPoll_;
    rrr::PollMgr *serverPoll_;
    rrr::ThreadPool *thrpool_;
    OOOBFTServiceImpl *oooService_;
    rrr::Server *oooServer_;
    std::thread *serverThread_;

    // concurrent queue for sending back deferred replies/ack to RPC calls
    ConcurrentQueue<rrr::DeferredReply *> replyQueue_;
    std::vector<std::thread> replyThreads_;

    // async watcher for receiving messages and adding them into the event loop.
    ev_async recvWatcher_;
    ConcurrentQueue<std::pair<std::string, Address>> recvQueue_;

    std::vector<Address> targetAddrs_;
    std::unordered_map<Address, std::vector<OOOProxyInfo>> proxies_;
    bool connected_ = false;

public:
    OOORPCEndpoint(
        const std::string &ip, const int port, const std::vector<Address> &targetAddrs, int numProxiesPerAddr = 5
    );
    ~OOORPCEndpoint();

    // Unlike UDP which is connectionless, for RPC based on TCP,
    // we must explicitly set up the connection before we want to send data to it
    void ConnectTo(const Address &dstAddr);

    void SetupServer();

    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;

    // use this function to set up my server and connect to my clients
    // The caller will call it after registering  handler
    virtual void LoopRun() override;

    // use this function to destroy my server context
    virtual void LoopBreak() override;

    void Connect();
};

#endif