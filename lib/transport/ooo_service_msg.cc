#include "ooo_service_msg.h"

namespace rrr {
Marshal &operator<<(Marshal &m, const OOOPrepareRequest &msg)
{
    m << msg.senderIPInt_;
    m << msg.senderPort_;
    m << msg.length_;
    m.write(msg.content_.data(), msg.length_);
    return m;
}

Marshal &operator>>(Marshal &m, OOOPrepareRequest &msg)
{
    m >> msg.senderIPInt_;
    m >> msg.senderPort_;
    m >> msg.length_;
    msg.content_.resize(msg.length_, '\0');
    m.read(&(msg.content_[0]), msg.length_);
    return m;
}
}   // namespace rrr
