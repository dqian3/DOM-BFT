#include "lib/transport/endpoint.h"

Endpoint::Endpoint(const bool isMasterReceiver, const std::optional<Address> &loopbackAddr)
    : loopbackAddr_(loopbackAddr)
{
    bzero(sendBuffer_, sizeof(sendBuffer_));
    evLoop_ = isMasterReceiver ? ev_default_loop() : ev_loop_new();
    if (!evLoop_) {
        LOG(ERROR) << "Event Loop error";
        return;
    }
    memset(sendBuffer_, 0, SEND_BUFFER_SIZE);
}

Endpoint::~Endpoint()
{
    LoopBreak();
    ev_loop_destroy(evLoop_);
}

bool Endpoint::RegisterTimer(Timer *timer)
{
    if (evLoop_ == NULL) {
        LOG(ERROR) << "No evLoop!";
        return false;
    }

    if (isTimerRegistered(timer)) {
        LOG(ERROR) << "This timer has already been registered";
        return false;
    }

    timer->attachedEndpoint_ = this;
    eventTimers_.insert(timer);
    ev_timer_again(evLoop_, timer->evTimer_);
    return true;
}

bool Endpoint::ResetTimer(Timer *timer)
{
    if (evLoop_ == NULL) {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    if (!isTimerRegistered(timer)) {
        LOG(ERROR) << "The timer has not been registered ";
        return false;
    }
    ev_timer_again(evLoop_, timer->evTimer_);
    return true;
}

bool Endpoint::ResetTimer(Timer *timer, uint32_t timeout_us)
{
    timer->evTimer_->repeat = timeout_us * 1e-6;
    return ResetTimer(timer);
}

bool Endpoint::UnRegisterTimer(Timer *timer)
{
    if (evLoop_ == NULL) {
        LOG(ERROR) << "No evLoop!";
        return false;
    }
    if (!isTimerRegistered(timer)) {
        LOG(ERROR) << "The timer has not been registered ";
        return false;
    }
    ev_timer_stop(evLoop_, timer->evTimer_);
    eventTimers_.erase(timer);
    return true;
}

bool Endpoint::PauseTimer(Timer *timerToPause, uint32_t pauseTime_us)
{
    uint64_t remaining = GetTimerRemaining(timerToPause);

    if (remaining == 0) {
        return false;
    }

    return ResetTimer(timerToPause, remaining + pauseTime_us);
}

void Endpoint::UnRegisterAllTimers()
{
    for (auto &t : eventTimers_) {
        ev_timer_stop(evLoop_, t->evTimer_);
    }
    eventTimers_.clear();
}

uint64_t Endpoint::GetTimerRemaining(Timer *t)
{
    if (evLoop_ == NULL) {
        LOG(ERROR) << "No evLoop!";
        return 0;
    }
    if (!isTimerRegistered(t)) {
        LOG(ERROR) << "The timer has not been registered ";
        return 0;
    }
    return 1e+6 * ev_timer_remaining(evLoop_, t->evTimer_);
}

bool Endpoint::isTimerRegistered(Timer *timer) { return (eventTimers_.find(timer) != eventTimers_.end()); }

MessageHeader *Endpoint::PrepareMsg(const byte *msg, u_int32_t msgLen, byte msgType, byte *buf, size_t bufSize)
{
    // If no buffer is given, use our own
    if (buf == nullptr) {
        buf = sendBuffer_;
        bufSize = SEND_BUFFER_SIZE;
    }

    MessageHeader *hdr = (MessageHeader *) buf;
    hdr->msgType = msgType;
    hdr->msgLen = msgLen;
    hdr->sigLen = 0;
    if (msgLen + sizeof(MessageHeader) > SEND_BUFFER_SIZE) {
        LOG(ERROR) << "Msg too large " << (uint32_t) msgType << "\t length=" << msgLen;
        return nullptr;
    }
    memcpy(buf + sizeof(MessageHeader), msg, hdr->msgLen);

    return hdr;
}

MessageHeader *Endpoint::PrepareProtoMsg(const google::protobuf::Message &msg, byte msgType, byte *buf, size_t bufSize)
{
    // If no buffer is given, use our own
    if (buf == nullptr) {
        buf = sendBuffer_;
        bufSize = SEND_BUFFER_SIZE;
    }

    MessageHeader *hdr = (MessageHeader *) buf;
    hdr->msgType = msgType;
    hdr->msgLen = msg.ByteSizeLong();
    hdr->sigLen = 0;

    if (hdr->msgLen + sizeof(MessageHeader) > bufSize) {
        LOG(ERROR) << "Msg too large " << (uint32_t) msgType << "\t length=" << hdr->msgLen;
        return nullptr;
    }
    msg.SerializeToArray(buf + sizeof(MessageHeader), hdr->msgLen);

    return hdr;
}

void Endpoint::LoopRun() { ev_run(evLoop_, 0); }

void Endpoint::RegisterSignalHandler(SignalHandlerFunc signalHandler)
{
    signalWatcher_.data = this;
    signalHandler_ = signalHandler;

    // TODO handle other than SIGINT?
    ev_signal_init(
        &signalWatcher_,
        [](struct ev_loop *loop, ev_signal *w, int revents) {
            Endpoint *e = (Endpoint *) w->data;
            e->signalHandler_();
        },
        SIGINT
    );
    ev_signal_start(evLoop_, &signalWatcher_);
}

void Endpoint::LoopBreak()
{
    UnRegisterAllTimers();
    ev_break(evLoop_, EVBREAK_ALL);
}
