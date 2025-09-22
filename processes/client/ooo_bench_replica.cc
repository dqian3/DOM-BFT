#include "ooo_bench_replica.h"

OOOBenchReplica::OOOBenchReplica(int replyTdNum)
{
    running_ = false;
    replyTdNum_ = replyTdNum;
    replyTds_.resize(replyTdNum, NULL);
    qu_.resize(replyTdNum);
}

OOOBenchReplica::~OOOBenchReplica() {}

void OOOBenchReplica::OnOOOBenchRequest(const OOOBenchRequest &req, OOOBenchReply *rep, rrr::DeferredReply *defer)
{
    OOOBenchEntry *entry = new OOOBenchEntry();
    entry->req_ = req;
    entry->rep_ = rep;
    entry->hdl_ = defer;
    qu_[req.reqId_ % replyTdNum_].enqueue(entry);
}

void OOOBenchReplica::ReplyTd(int id)
{
    OOOBenchEntry *entry;
    while (running_) {
        while (qu_[id].try_dequeue(entry)) {
            OOOBenchReply *rep = entry->rep_;
            rep->clientId_ = entry->req_.clientId_;
            rep->reqId_ = entry->req_.reqId_;
            rep->content_ = entry->req_.content_;
            entry->hdl_->reply();
        }
    }
}

void OOOBenchReplica::Run()
{
    running_ = true;
    for (int i = 0; i < replyTdNum_; i++) {
        replyTds_[i] = new std::thread(&OOOBenchReplica::ReplyTd, this, i);
    }
}

void OOOBenchReplica::Stop()
{
    running_ = false;
    for (int i = 0; i < replyTdNum_; i++) {
        replyTds_[i]->join();
        delete replyTds_[i];
        replyTds_[i] = NULL;
    }
}