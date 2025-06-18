#pragma once

#include "lib/rrr/rrr.hpp"

#include <errno.h>

// clang-format off

// optional %%: marks header section, code above will be copied into begin of generated C++ header
namespace OOO_BFT_RPC {

class OOOBFTService: public rrr::Service {
public:
    enum {
        SENDOOOPREPAREREQUEST = 0x1a6dc960,
    };
    int __reg_to__(rrr::Server* svr) {
        int ret = 0;
        if ((ret = svr->reg(SENDOOOPREPAREREQUEST, this, &OOOBFTService::__SendOOOPrepareRequest__wrapper__)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr->unreg(SENDOOOPREPAREREQUEST);
        return ret;
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, remember to reply req, delete req, and sconn->release(); use sconn->run_async for heavy job
    virtual void SendOOOPrepareRequest(const OOOPrepareRequest& req, rrr::DeferredReply* defer) = 0;
private:
    void __SendOOOPrepareRequest__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        OOOPrepareRequest* in_0 = new OOOPrepareRequest;
        req->m >> *in_0;
        auto __marshal_reply__ = [=] {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->SendOOOPrepareRequest(*in_0, __defer__);
    }
};

class OOOBFTProxy {
protected:
    rrr::Client* __cl__;
public:
    OOOBFTProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::Future* async_SendOOOPrepareRequest(const OOOPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(OOOBFTService::SENDOOOPREPAREREQUEST, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 SendOOOPrepareRequest(const OOOPrepareRequest& req) {
        rrr::Future* __fu__ = this->async_SendOOOPrepareRequest(req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
};

} // namespace OOO_BFT_RPC


// optional %%: marks footer section, code below will be copied into end of generated C++ header


