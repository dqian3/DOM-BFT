#pragma once

#include "lib/rrr/rrr.hpp"

#include <errno.h>

// clang-format off

// optional %%: marks header section, code above will be copied into begin of generated C++ header
namespace OOO_BFT_RPC {

class OOOBFTBenchService: public rrr::Service {
public:
    enum {
        SENDOOOBENCHREQUEST = 0x45576c9a,
    };
    int __reg_to__(rrr::Server* svr) {
        int ret = 0;
        if ((ret = svr->reg(SENDOOOBENCHREQUEST, this, &OOOBFTBenchService::__SendOOOBenchRequest__wrapper__)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr->unreg(SENDOOOBENCHREQUEST);
        return ret;
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, remember to reply req, delete req, and sconn->release(); use sconn->run_async for heavy job
    virtual void SendOOOBenchRequest(const OOOBenchRequest& req, OOOBenchReply*, rrr::DeferredReply* defer) = 0;
private:
    void __SendOOOBenchRequest__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        OOOBenchRequest* in_0 = new OOOBenchRequest;
        req->m >> *in_0;
        OOOBenchReply* out_0 = new OOOBenchReply;
        auto __marshal_reply__ = [=] {
            *sconn << *out_0;
        };
        auto __cleanup__ = [=] {
            delete in_0;
            delete out_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->SendOOOBenchRequest(*in_0, out_0, __defer__);
    }
};

class OOOBFTBenchProxy {
protected:
    rrr::Client* __cl__;
public:
    OOOBFTBenchProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::Future* async_SendOOOBenchRequest(const OOOBenchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(OOOBFTBenchService::SENDOOOBENCHREQUEST, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 SendOOOBenchRequest(const OOOBenchRequest& req, OOOBenchReply* out_0) {
        rrr::Future* __fu__ = this->async_SendOOOBenchRequest(req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *out_0;
        }
        __fu__->release();
        return __ret__;
    }
};

} // namespace OOO_BFT_RPC


// optional %%: marks footer section, code below will be copied into end of generated C++ header


