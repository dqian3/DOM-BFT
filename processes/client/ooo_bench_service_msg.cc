#include "ooo_bench_service_msg.h"
namespace rrr {

Marshal &operator<<(Marshal &m, const OOOBenchRequest &msg) {
    m << msg.clientId_<< msg.reqId_ << msg.content_;
    return m;
}

Marshal &operator>>(Marshal &m, OOOBenchRequest &msg) {
    m >> msg.clientId_>>msg.reqId_>>msg.content_;
    return m;
}

Marshal &operator<<(Marshal &m, const OOOBenchReply &msg) {
    m << msg.clientId_<< msg.reqId_ << msg.content_;
    return m;
}

Marshal &operator>>(Marshal &m, OOOBenchReply &msg){
    m >> msg.clientId_>>msg.reqId_>>msg.content_;
    return m;
}
}