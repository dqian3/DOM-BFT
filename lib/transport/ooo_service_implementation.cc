#include "ooo_service_implementation.h"

namespace OOO_BFT_RPC {
OOOBFTServiceImpl::OOOBFTServiceImpl(const OOOHandler &h)
    : handler_(h)
{
}

void OOOBFTServiceImpl::SendOOOPrepareRequest(const std::string &req, rrr::DeferredReply *defer)
{
    handler_(req);
    // defer->reply triggers reply to the RPC client, if you do not want to reply immediately,
    // save the defer variable and pass it to the others
    defer->reply();
}
}   // namespace OOO_BFT_RPC