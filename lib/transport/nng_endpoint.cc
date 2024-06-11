#include "lib/transport/nng_endpoint.h"

NngMessageHandler::NngMessageHandler(MessageHandlerFunc msghdl, void *ctx)
    : MessageHandler(msghdl, ctx)
{
    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents)
            {
        NngMessageHandler* m = (NngMessageHandler*)(w->data);
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

NngMessageHandler::~NngMessageHandler() {}

NngEndpoint::NngEndpoint(const std::string &ip, const int port,
                         const bool isMasterReceiver)
    : Endpoint(isMasterReceiver), msgHandler_(NULL)
{
}

NngEndpoint::~NngEndpoint() {}

int UDPEndpoint::SendPreparedMsgTo(const Address &dstAddr)
{
    MessageHeader *hdr = (MessageHeader *)buffer_;

    if (!bufReady_)
    {
        LOG(ERROR) << "SendPreparedMsgTo called while bufferReady_ = false, make "
                   << "sure you call PrepareMsg() or PrepareProtoMsg() first ";
        return -1;
    }

    int ret = sendto(fd_, buffer_, sizeof(MessageHeader) + hdr->msgLen + hdr->sigLen, 0,
                     (struct sockaddr *)(&(dstAddr.addr_)), sizeof(sockaddr_in));
    if (ret < 0)
    {
        VLOG(1) << "\tSend Fail ret =" << ret;
    }

    if (!reuseBuffer)
        bufReady_ = false;

    return ret;
}


bool UDPEndpoint::RegisterMsgHandler(MessageHandler *msgHdl)
{
    UDPMessageHandler *udpMsgHdl = (UDPMessageHandler *)msgHdl;
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
    if (isMsgHandlerRegistered(msgHdl))
    {
        LOG(ERROR) << "This msgHdl has already been registered";
        return false;
    }
    msgHandler_ = udpMsgHdl;
    ev_io_set(udpMsgHdl->evWatcher_, fd_, EV_READ);
    ev_io_start(evLoop_, udpMsgHdl->evWatcher_);

    return true;
}

bool UDPEndpoint::UnRegisterMsgHandler(MessageHandler *msgHdl)
{
    UDPMessageHandler *udpMsgHdl = (UDPMessageHandler *)msgHdl;
    if (evLoop_ == NULL)
    {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    if (!isMsgHandlerRegistered(udpMsgHdl))
    {
        LOG(ERROR) << "The handler has not been registered ";
        return false;
    }
    ev_io_stop(evLoop_, udpMsgHdl->evWatcher_);
    msgHandler_ = NULL;
    return true;
}

bool UDPEndpoint::isMsgHandlerRegistered(MessageHandler *msgHdl)
{
    return (UDPMessageHandler *)msgHdl == msgHandler_;
}

void UDPEndpoint::UnRegisterAllMsgHandlers()
{
    ev_io_stop(evLoop_, msgHandler_->evWatcher_);
    msgHandler_ = NULL;
}
