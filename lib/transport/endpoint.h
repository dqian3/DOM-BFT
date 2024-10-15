#ifndef ENDPOINT_H
#define ENDPOINT_H

#include "lib/common_struct.h"
#include "lib/transport/address.h"
#include "lib/transport/message_handler.h"
#include "lib/transport/timer.h"
#include <arpa/inet.h>
#include <ev.h>
#include <fcntl.h>
#include <functional>
#include <glog/logging.h>
#include <google/protobuf/message.h>
#include <netinet/in.h>
#include <set>
#include <string>

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
class Endpoint {
protected:
    /** The ev_loop struct from libev, which uses to handle io/timer events */
    struct ev_loop *evLoop_;
    /** One endpoint can have multiple timers registered. We maintain a set to
     * avoid duplicate registration and check whether a specific timer has been
     * registered or not.*/
    std::set<struct Timer *> eventTimers_;

    byte sendBuffer_[SEND_BUFFER_SIZE];

public:
    int epId_;   // The id of the endpoint, mainly for debug

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

    bool ResetTimer(Timer *timer);
    bool ResetTimer(Timer *timer, uint32_t timeout_us);

    /** Return true if the timer is successfully unregistered, otherwise (e.g. the
     * timer has not been registered before), return false */
    bool UnRegisterTimer(Timer *timer);
    /** Check whether the timer has been registered */
    bool isTimerRegistered(Timer *timer);

    // temporarily pause the timer, and restart after a while.
    // the restart is handled by the second timer in the parameter list
    bool PauseTimer(Timer *timerToPause, uint32_t pauseTime);

    uint64_t GetTimerRemaining(Timer *timer);

    void UnRegisterAllTimers();

    // -------------------- Message Sending --------------------

    // Loads message into buf and prepends header
    // If nullptr, use own buffer
    // TODO use some more sophisticated memory allocation scheme?
    MessageHeader *PrepareMsg(const byte *msg, u_int32_t msgLen, byte msgType, byte *buf = nullptr,
                              size_t bufSize = SEND_BUFFER_SIZE);

    MessageHeader *PrepareProtoMsg(const google::protobuf::Message &msg, const byte msgType, byte *buf = nullptr,
                                   size_t bufSize = SEND_BUFFER_SIZE);

    // Sends message in buffer to address specifed in dstAddr.
    // Note that the MessageHeader that PrepareMsg creates contains
    // the size information needed.

    // The reason preparing and sending messages are split is so for
    // broadcast we can reuse the same buffer/signatures
    virtual int SendPreparedMsgTo(const Address &dstAddr, byte *buf = nullptr) = 0;

    // -------------------- Entrypoints --------------------
    void LoopRun();
    void LoopBreak();
};

#endif