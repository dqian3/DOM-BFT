#include "kv_rocksdb.h"
#include <rocksdb/slice.h>
#include <rocksdb/options.h>
#include <rocksdb/utilities/checkpoint.h>


KVStore::KVStore(const std::string &dbPath) {
    rocksdb::Options options;
    options.create_if_missing = true;
    rocksdb::Status status = rocksdb::DB::Open(options, dbPath, &db_);
    if (!status.ok()) {
        std::cerr << "Failed to open database: " << status.ToString() << std::endl;
        exit(1);
    }
}

KVStore::~KVStore() {
    delete db_;
}

bool KVStore::set(const std::string &key, const std::string &value) {
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value);
    return status.ok();
}

bool KVStore::get(const std::string &key, std::string *value) {
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, value);
    return status.ok();
}

bool KVStore::del(const std::string &key) {
    rocksdb::Status status = db_->Delete(rocksdb::WriteOptions(), key);
    return status.ok();
}

const rocksdb::Snapshot *KVStore::snapshot() {
    return db_->GetSnapshot();
}

void KVStore::rollback(const rocksdb::Snapshot* snapshot) {
    if (snapshot) {
        db_->ReleaseSnapshot(snapshot);
    }
}
