#include "kv_rocksdb.h"
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/checkpoint.h>
#include <glog/logging.h>


KVStore::KVStore(const std::string &dbPath) {
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::Status status = rocksdb::TransactionDB::Open(options_, txn_db_options_, dbPath, &db_);
    if (!status.ok()) {
        LOG(ERROR) << "Failed to open transactional database: " << status.ToString();
        exit(1);
    }

    // to prevent seg fault：
    txn_ = nullptr;
}

KVStore::~KVStore() {
    delete db_;
}

void KVStore::beginTransaction() {
    LOG(INFO) << "Transaction beginning";
    if (txn_ != nullptr) {
        delete txn_; // Clean up any existing transaction
    }
    LOG(INFO) << "Transaction check null finished"; 
    txn_ = db_->BeginTransaction(rocksdb::WriteOptions());
}

bool KVStore::set(const std::string &key, const std::string &value) {
    if (!txn_) {
        LOG(INFO) << "Transaction not started";
        return false;
    }
    rocksdb::Status status = txn_->Put(key, value);
    return status.ok();
}

bool KVStore::get(const std::string &key, std::string *value) {
    if (!txn_) {
        LOG(INFO) << "Transaction not started";
        return false;
    }
    rocksdb::Status status = txn_->Get(rocksdb::ReadOptions(), key, value);
    return status.ok();
}

bool KVStore::del(const std::string &key) {
    if (!txn_) {
        LOG(INFO) << "Transaction not started";
        return false;
    }
    rocksdb::Status status = txn_->Delete(key);
    return status.ok();
}

// const rocksdb::Snapshot *KVStore::snapshot() {
//     return db_->GetSnapshot();
// }

void KVStore::rollback() {
    if (txn_) {
        txn_->Rollback();
        delete txn_;
        txn_ = nullptr;
    }
}

bool KVStore::commit() {
    if (txn_) {
        LOG(INFO) << "Transaction committing";
        rocksdb::Status status = txn_->Commit();
        LOG(INFO) << "Transaction committed";
        if (!status.ok()) {
            LOG(INFO) << "Transaction commit failed: " << status.ToString();
        }
        delete txn_;
        txn_ = nullptr;

        return true;
    }
    LOG(WARNING) << "No transaction to commit.";
    return false;
}