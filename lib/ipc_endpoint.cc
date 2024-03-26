#include "lib/ipc_endpoint.h"

#include <sys/un.h>

IPCMessageHandler::IPCMessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL)
    : MessageHandler(msghdl, ctx)
{
    ev_init(evWatcher_, [] (struct ev_loop *loop, struct ev_io *w, int revents) 
    {
        IPCMessageHandler* m = (IPCMessageHandler*)(w->data);

        // We shouldn't need the IPC sender address, our IPC is 1 to 1
        // TODO, we should make this the case if not, and add support for IPC in Address if so
        int msgLen = recv(w->fd, m->buffer_, IPC_BUFFER_SIZE, 0);

        if (msgLen > 0 && (uint32_t)msgLen > sizeof(MessageHeader)) 
        {
            MessageHeader* msgHeader = (MessageHeader*)(void*)(m->buffer_);
            if (msgHeader->msgLen + sizeof(MessageHeader) >= (uint32_t)msgLen) 
            {
                m->msgHandler_(msgHeader, m->buffer_ + sizeof(MessageHeader),
                                nullptr, m->context_);
            }
        } 
    });
}

IPCMessageHandler::~IPCMessageHandler()
{

}

IPCEndpoint::IPCEndpoint(const std::string &ipcAddr,
                         const bool isMasterReceiver)
    : Endpoint(isMasterReceiver), msgHandler_(NULL)
{
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };

    fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    strcpy(addr.sun_path, ipcAddr.c_str()); // TODO check this isn't too big
    unlink(addr.sun_path);

    int len = strlen(addr.sun_path) + sizeof(addr.sun_family);

    bind(fd_, (struct sockaddr *)&addr, len);
}

IPCEndpoint::~IPCEndpoint() {}

// This is basically the same as UDP, could be consolidated...
int IPCEndpoint::SendMsgTo(const std::string &dstAddr,
                           const char *msg,
                           u_int32_t msgLen,
                           char msgType)
{
    char buffer[IPC_BUFFER_SIZE];
    MessageHeader *msgHdr = (MessageHeader *)(void *)buffer;
    msgHdr->msgType = msgType;
    msgHdr->msgLen = msgLen;
    if (msgLen + sizeof(MessageHeader) > IPC_BUFFER_SIZE)
    {
        LOG(ERROR) << "Msg too large " << (uint32_t)msgType
                   << "\t length=" << msgLen;
        return -1;
    }

    memcpy(buffer + sizeof(MessageHeader), msg,
           msgHdr->msgLen);

    // TODO don't do this every time, it's probably fine isn't too big
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };

    fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    strcpy(addr.sun_path, dstAddr.c_str());

    int ret = sendto(fd_, buffer, msgHdr->msgLen + sizeof(MessageHeader), 0,
                     (struct sockaddr *)(&(addr)), sizeof(sockaddr_in));
    if (ret < 0)
    {
        VLOG(1) << pthread_self() << "\tSend Fail ret =" << ret;
    }
    return ret;
}

int IPCEndpoint::SendProtoMsgTo(const std::string &dstAddr,
                                const google::protobuf::Message &msg,
                                char msgType)
{
    std::string serializedString = msg.SerializeAsString();
    uint32_t msgLen = serializedString.length();
    if (msgLen > 0)
    {
        SendMsgTo(dstAddr, serializedString.c_str(), msgLen, msgType);
    }
    return -1;
}

bool IPCEndpoint::RegisterMsgHandler(MessageHandler *msgHdl)
{
    IPCMessageHandler *ipcMsgHdl = (IPCMessageHandler *)msgHdl;
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

    msgHandler_ = ipcMsgHdl;
    ev_io_set(ipcMsgHdl->evWatcher_, fd_, EV_READ);
    ev_io_start(evLoop_, ipcMsgHdl->evWatcher_);

    return true;
}

bool IPCEndpoint::UnRegisterMsgHandler(MessageHandler *msgHdl)
{
    IPCMessageHandler *ipcMsgHdl = (IPCMessageHandler *)msgHdl;
    if (evLoop_ == NULL)
    {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    if (!isMsgHandlerRegistered(ipcMsgHdl))
    {
        LOG(ERROR) << "The handler has not been registered ";
        return false;
    }
    ev_io_stop(evLoop_, udpMsgHdl->evWatcher_);
    msgHandler_ = NULL;
    return true;
}

bool IPCEndpoint::isMsgHandlerRegistered(MessageHandler *msgHdl)
{
    return (IPCMessageHandler *)msgHdl == msgHandler_;
}

void IPCEndpoint::UnRegisterAllMsgHandlers()
{
    ev_io_stop(evLoop_, msgHandler_->evWatcher_);
    msgHandler_ = NULL;
}
