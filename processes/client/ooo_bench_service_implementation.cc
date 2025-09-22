#include "ooo_bench_service_implementation.h"

namespace OOO_BFT_RPC {
OOOBFTBenchServiceImpl::OOOBFTBenchServiceImpl(OOOBenchReplica *replica) { replica_ = replica; }
void OOOBFTBenchServiceImpl::SendOOOBenchRequest(
    const OOOBenchRequest &req, OOOBenchReply *rep, rrr::DeferredReply *defer
)
{
    replica_->OnOOOBenchRequest(req, rep, defer);
}
void OOOBFTBenchServiceImpl::StopReplicaProcessingThreads() { replica_->Stop(); }
}   // namespace OOO_BFT_RPC