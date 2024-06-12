#include "lib/transport/nng_endpoint.h"


NngMessageHandler::NngMessageHandler(MessageHandlerFunc msghdl, void *ctx)
    : msgHandler_(msghdl), context_(ctx)
{
    evWatcher_ = new ev_io();
    evWatcher_->data = (void *)this;

    ev_init(evWatcher_, [](struct ev_loop *loop, struct ev_io *w, int revents) {
        NngMessageHandler* m = (NngMessageHandler*)(w->data);
        // TODO
    });
}

NngMessageHandler::~NngMessageHandler() {}

NngEndpoint::NngEndpoint(const std::vector<Address> &bindAddrs, const std::vector<Address> &sendAddrs, bool isMasterReceiver)
    : Endpoint(isMasterReceiver)
{
    // TODO
}

NngEndpoint::~NngEndpoint() {}

int NngEndpoint::SendPreparedMsgTo(const Address &dstAddr)
{
    MessageHeader *hdr = (MessageHeader *)sendBuffer_;

    // TODO

    return 0;
}

bool NngEndpoint::RegisterMsgHandler(MessageHandlerFunc hdl)
{

    // TODO bind separate NngMessageHandler to each address

    return true;
}
