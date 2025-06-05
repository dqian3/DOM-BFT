
#include "event.h"
#include "coroutine.h"
#include "scheduler.h"

namespace rrr {

void Event::Wait()
{
    // TODO
    if (IsReady()) {
        return;
    } else {
        coro_->Yield();
    }
    rrr_verify(0);
}

bool IntEvent::TestTrigger()
{
    rrr_verify(status_ <= WAIT);
    if (value_ == target_) {
        status_ = READY;
        sched_->AddReadyEvent(this);
        return true;
    }
    return false;
}

}   // namespace rrr