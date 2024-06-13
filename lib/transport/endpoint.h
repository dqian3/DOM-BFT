#ifndef ENDPOINT_H
#define ENDPOINT_H

#include <arpa/inet.h>
#include <ev.h>
#include <fcntl.h>
#include <glog/logging.h>
#include <google/protobuf/message.h>
#include <netinet/in.h>
#include <functional>
#include <set>
#include <string>
#include "lib/transport/address.h"
#include "lib/common_struct.h"
#include "lib/transport/message_handler.h"
#include "lib/transport/timer.h"

/**
 * Endpoint is the basic abstraction we use for communcation, and can be
 * derived to handle different communication primtives such as UDP/IPC.
 *
 *
 * Essentially, it serves as a wrapper around a socket + libev
 * An Endpoint supports three major functionalities:
 * (1) Receive messages;
 * (2) Process the received messages according to a customized
 * message handler
 * (3) Conduct periodic actions according to (pre-registered)
 * customized timer functions.
 * 
 * For convenience, we also have the endpoint 
 * (4) Provide a buffer and an interface for sending a message to a specified address
 */
class Endpoint
{
protected:
    /** The socket fd it uses to send/recv messages */
    int fd_;
    /** The ev_loop struct from libev, which uses to handle io/timer events */
    struct ev_loop *evLoop_;
    /** One endpoint can have multiple timers registered. We maintain a set to
     * avoid duplicate registration and check whether a specific timer has been
     * registered or not.*/
    std::set<struct Timer *> eventTimers_;

    byte sendBuffer_[SEND_BUFFER_SIZE];

public:
    int epId_; // The id of the endpoint, mainly for debug

    Endpoint(const bool isMasterReceiver = false);
    virtual ~Endpoint();


    // -------------------- Socket/IO Handling --------------------

    /* We have a very simple interface for Message Handling, a single
     * callback is given and any messages received on this endpoint
     * go to it.
     */
    virtual bool RegisterMsgHandler(MessageHandlerFunc msgHdl) = 0;


    // -------------------- Timer Handling --------------------
    // The timer handling is slightly more complicated, since we may need to 
    // register and destroy timers

    /** Return true if the timer is successfully registered, otherwise (e.g. it
     * has been registered before and has not been unreigstered), return false */
    bool RegisterTimer(Timer *timer);
    /** Return true if the timer is successfully unregistered, otherwise (e.g. the
     * timer has not been registered before), return false */
    bool UnRegisterTimer(Timer *timer);
    /** Check whether the timer has been registered */
    bool isTimerRegistered(Timer *timer);
    void UnRegisterAllTimers();


    // -------------------- Message Sending --------------------

    // Loads message with header prepended into buffer and sets
    // bufReady to true. TODO get some info about buffer size.
    MessageHeader *PrepareMsg(const byte *msg,
                  u_int32_t msgLen,
                  byte msgType);

    MessageHeader *PrepareProtoMsg(
        const google::protobuf::Message &msg,
        const byte msgType);

    // Sends message in buffer to address specifed in dstAddr.
    // Note that the MessageHeader that PrepareMsg creates contains
    // the size information needed.

    // The reason preparing and sending messages are split is so for
    // broadcast we can reuse the same buffer/signatures
    virtual int SendPreparedMsgTo(const Address &dstAddr) = 0;

    // -------------------- Entrypoints --------------------
    void LoopRun();
    void LoopBreak();
};

#endif