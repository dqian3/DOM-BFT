#include "lib/transport/endpoint.h"

Endpoint::Endpoint(const bool isMasterReceiver)
{
    evLoop_ = isMasterReceiver ? ev_default_loop() : ev_loop_new();
    if (!evLoop_) {
        LOG(ERROR) << "Event Loop error";
        return;
    }
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


// the restart timer will restart the timerToPause when the timer expires. 
bool Endpoint::PauseTimer(Timer *timerToPause, uint32_t pauseTime)
{
    if (evLoop_ == NULL) {
        LOG(ERROR) << "No evLoop!";
        return false;
    } 

    if (!isTimerRegistered(timerToPause)) {
        LOG(ERROR) << "The timer to pause here has not been registered ";
        return false; 
    }

    ev_timer_stop(evLoop_, timerToPause->evTimer_);

    LOG(INFO) << "the timer has been stopeed";

    auto restartTimer = new ev_timer();

    restartTimer->data = timerToPause;

    auto resume_callback = [](struct ev_loop *loop, ev_timer *w, int revents) {
        LOG(INFO) << "resuming the paused timer";

        Timer* timerToResume = static_cast<Timer*>(w->data);
        ev_timer_again(loop, timerToResume->evTimer_);
        ev_timer_stop(loop, w);
    };

    ev_timer_init(restartTimer, resume_callback, pauseTime, 0);

    ev_timer_start(evLoop_, restartTimer);

    LOG(INFO) << "restart timer initiated";

    return true;
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
        return false;
    }
    if (!isTimerRegistered(t)) {
        LOG(ERROR) << "The timer has not been registered ";
        return false;
    }
    return 1e+6 * ev_timer_remaining(evLoop_, t->evTimer_);
}

bool Endpoint::isTimerRegistered(Timer *timer) { return (eventTimers_.find(timer) != eventTimers_.end()); }

MessageHeader *Endpoint::PrepareMsg(const byte *msg, u_int32_t msgLen, byte msgType)
{
    MessageHeader *hdr = (MessageHeader *) sendBuffer_;
    hdr->msgType = msgType;
    hdr->msgLen = msgLen;
    hdr->sigLen = 0;
    if (msgLen + sizeof(MessageHeader) > SEND_BUFFER_SIZE) {
        LOG(ERROR) << "Msg too large " << (uint32_t) msgType << "\t length=" << msgLen;
        return nullptr;
    }

    memcpy(sendBuffer_ + sizeof(MessageHeader), msg, hdr->msgLen);

    return hdr;
}

MessageHeader *Endpoint::PrepareProtoMsg(const google::protobuf::Message &msg, byte msgType)
{
    MessageHeader *hdr = (MessageHeader *) sendBuffer_;
    hdr->msgType = msgType;
    hdr->msgLen = msg.ByteSizeLong();
    hdr->sigLen = 0;

    if (hdr->msgLen + sizeof(MessageHeader) > SEND_BUFFER_SIZE) {
        LOG(ERROR) << "Msg too large " << (uint32_t) msgType << "\t length=" << hdr->msgLen;
        return nullptr;
    }
    msg.SerializeToArray(sendBuffer_ + sizeof(MessageHeader), hdr->msgLen);

    return hdr;
}

void Endpoint::LoopRun() { ev_run(evLoop_, 0); }

void Endpoint::LoopBreak()
{
    UnRegisterAllTimers();
    ev_break(evLoop_, EVBREAK_ALL);
}
