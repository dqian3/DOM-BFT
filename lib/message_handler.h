
#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <arpa/inet.h>
#include <ev.h>
#include <netinet/in.h>
#include <functional>
#include "lib/address.h"
#include "lib/common_struct.h"

/**
 * MessageHandler is an encapsulation of libev-based message handler (i.e.
 * ev_io).
 *
 * After the message handler is created, it will be registered to a
 * specific endpoint. Then, the callback func (i.e., MessageHandlerFunc) will be
 * called every time this endpoint receives some messages.
 *
 * Currently, we only support UDP and Unix Socket IPC communication. Therefore, 
 * we only have two derived structs (UDPMessageHandler and IPCMsgHandler) from 
 * MessageHandler. These are defined alongside the respective endpoint types.
 *
 * **/

/**
 * MessageHeader* describes the type and length of the received message
 * byte* is the payload of the message
 * Address* is the address of the sender
 * void* points to the (optional) context that is needed by the callback
 * function, such as the this pointer
 */
typedef std::function<void(MessageHeader *, byte *, Address *, void *)>
    MessageHandlerFunc;

struct MessageHandler
{
    MessageHandlerFunc msgHandler_;
    void *context_;
    Address sender_;
    struct ev_io *evWatcher_;
    MessageHandler(MessageHandlerFunc msghdl, void *ctx = NULL)
        : msgHandler_(msghdl), context_(ctx)
    {
        evWatcher_ = new ev_io();
        evWatcher_->data = (void *)this;
    }

    // Pure virtual, since this class shouldn't be used by itself
    // See .cc for details.
    virtual ~MessageHandler() = 0;
};

#endif