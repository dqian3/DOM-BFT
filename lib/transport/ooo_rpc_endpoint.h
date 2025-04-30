#ifndef OOO_RPC_ENDPOINT_H
#define OOO_RPC_ENDPOINT_H

#include "lib/transport/endpoint.h"
#include "lib/transport/ooo_service_implementation.h"

void OOORPCMsgHandler(const std::string &msg) { printf("The message is %s", msg.c_str()); }

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
    MessageHandlerFunc hdlrFunc_;
    OOOHandler oooHdl_;

    rrr::PollMgr *clientPoll_;
    rrr::PollMgr *serverPoll_;
    rrr::ThreadPool *thrpool_;
    OOOBFTServiceImpl *oooService_;
    rrr::Server *oooServer_;
    std::thread *serverThread_;

    std::unordered_map<Address, OOOProxyInfo> proxies_;

public:
    OOORPCEndpoint(const std::string &ip, const int port, const OOOHandler &hdl);
    ~OOORPCEndpoint();

    // Unlike UDP which is connectionless, for RPC based on TCP,
    // we must actively set up the connection before we want to send data to it
    void ConnectTo(const Address &dstAddr);

    void SetupServer();

    // Sends message in buffer
    virtual int SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) override;

    virtual bool RegisterMsgHandler(MessageHandlerFunc) override;
};

#endif