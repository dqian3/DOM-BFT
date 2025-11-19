#include "ooo_service_implementation.h"

namespace OOO_BFT_RPC {
OOOBFTServiceImpl::OOOBFTServiceImpl(const OOOHandler &h)
    : handler_(h)
{
}

void OOOBFTServiceImpl::SendOOOPrepareRequest(const OOOPrepareRequest &req, rrr::DeferredReply *defer)
{
    handler_(req, defer);
}
}   // namespace OOO_BFT_RPC