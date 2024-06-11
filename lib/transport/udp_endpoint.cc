#include "lib/transport/udp_endpoint.h"

UDPMessageHandler::UDPMessageHandler(MessageHandlerFunc msghdl, void *ctx)
    : MessageHandler(msghdl, ctx)
{
    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents)
            {
        UDPMessageHandler* m = (UDPMessageHandler*)(w->data);
        socklen_t sockLen = sizeof(struct sockaddr_in);
        
        int msgLen = recvfrom(w->fd, m->buffer_, UDP_BUFFER_SIZE, 0,
                                (struct sockaddr*)(&(m->sender_.addr_)), &sockLen);
        if (msgLen > 0 && (uint32_t)msgLen > sizeof(MessageHeader)) 
        {
            MessageHeader* msgHeader = (MessageHeader*)(void*)(m->buffer_);
            if (sizeof(MessageHeader) + msgHeader->msgLen + msgHeader->sigLen >= (uint32_t)msgLen) 
            {
                m->msgHandler_(msgHeader, m->buffer_ + sizeof(MessageHeader),
                                &(m->sender_), m->context_);
            }
        } });
}

UDPMessageHandler::~UDPMessageHandler() {}

UDPEndpoint::UDPEndpoint(const std::string &ip, const int port,
                         const bool isMasterReceiver)
    : Endpoint(isMasterReceiver), msgHandler_(NULL)
{
    fd_ = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0)
    {
        LOG(ERROR) << "Receiver Fd fail ";
        return;
    }
    // Set Non-Blocking
    int status = fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0)
    {
        LOG(ERROR) << " Set NonBlocking Fail";
    }
    if (ip == "" || port < 0)
    {
        return;
    }

    bound_ = true;
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    // Bind socket to Address
    int bindRet = bind(fd_, (struct sockaddr *)&addr, sizeof(addr));
    if (bindRet != 0)
    {
        LOG(ERROR) << "bind error\t" << bindRet << "\t port=" << port;
        return;
    }
}

UDPEndpoint::~UDPEndpoint() {}

int UDPEndpoint::SendPreparedMsgTo(const Address &dstAddr)
{
    MessageHeader *hdr = (MessageHeader *)buffer_;

    int ret = sendto(fd_, buffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0,
                     (struct sockaddr *)(&(dstAddr.addr_)), sizeof(sockaddr_in));
    if (ret < 0)
    {
        VLOG(1) << "\tSend Fail ret =" << ret;
    }

    return ret;
}

bool UDPEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{
    if (!bound_)
    {
        LOG(ERROR) << "Endpoint is not bound!";
        return false;
    }
    if (evLoop_ == NULL)
    {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    msgHandler_ = std::make_unique<UDPMessageHandler>(hdl);
    ev_io_set(msgHandler_->evWatcher_, fd_, EV_READ);
    ev_io_start(evLoop_, msgHandler_->evWatcher_);

    return true;
}
