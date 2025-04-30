#include "ooo_rpc_endpoint.h"

OOORPCEndpoint::OOORPCEndpoint(const std::string &ip, const int port, const OOOHandler &hdl)
    : myIP_(ip)
    , myListeningPort_(port)
{
    hdlrFunc_ = NULL;
    clientPoll_ = new rrr::PollMgr(2);
    serverPoll_ = new rrr::PollMgr(2);
    thrpool_ = new rrr::ThreadPool(8);
}

OOORPCEndpoint::~OOORPCEndpoint() {}

void OOORPCEndpoint::ConnectTo(const Address &dstAddr)
{
    if (proxies_.find(dstAddr) != proxies_.end()) {
        LOG(INFO) << "Already Connected " << dstAddr.ip_ << ":" << dstAddr.port_;
        return;
    }
    LOG(INFO) << "ConnectTo= " << dstAddr.ip_ << ":" << dstAddr.port_;
    int ret = -1;
    rrr::Client *client = new rrr::Client(clientPoll_);
    do {
        std::string addrString = dstAddr.ip_ + ":" + std::to_string(dstAddr.port_);
        ret = client->connect(addrString.c_str());
        if (ret == 0) {
            // success
            LOG(INFO) << "Sucessful Connection" << addrString;
        } else {
            sleep(1);
        }
    } while (ret != 0);

    OOOBFTProxy *proxy = new OOOBFTProxy(client);
    // Record this proxy info
    proxies_[dstAddr] = {client, proxy};
}

void OOORPCEndpoint::SetupServer()
{
    // Use different thread, do not block the caller
    serverThread_ = new std::thread([this] {
        std::string myServerAddr = myIP_ + ":" + std::to_string(myListeningPort_);
        LOG(INFO) << "Start Service serverAddr=" << myServerAddr;
        oooServer_->start(myServerAddr.c_str());
    });
}

int OOORPCEndpoint::SendPreparedMsgTo(const Address &dstAddr, MessageHeader *hdr) {}

bool OOORPCEndpoint::RegisterMsgHandler(MessageHandlerFunc f)
{
    if (hdlrFunc_) {
        LOG(INFO) << "handler already set, do not overwrite it (causing data race)";
        return false;
    }
    hdlrFunc_ = f;
    oooHdl_ = [this](const std::string &str) {
        // Parse the string to MessageHeader style
        MessageHeader *header;
        byte *payload;
        // Delegate to the hdlrFunc_;
        hdlrFunc_(header, payload, NULL /** Cannot get the address */);
    };
    oooServer_ = new rrr::Server(serverPoll_, thrpool_);
    OOOBFTServiceImpl *oooService_ = new OOOBFTServiceImpl(oooHdl_);
    oooServer_->reg(oooService_);

    return true;
}