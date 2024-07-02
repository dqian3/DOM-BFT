#ifndef KV_STORE_H
#define KV_STORE_H

#include <rocksdb/db.h>
#include <rocksdb/utilities/transaction_db.h>   
#include <string>
#include <iostream> 

class KVStore {
public:
    KVStore(const std::string &dbPath);
    ~KVStore();

    void beginTransaction();

    bool set(const std::string &key, const std::string &value);
    bool get(const std::string &key, std::string *value);
    bool del(const std::string &key);
    // const rocksdb::Snapshot *snapshot();
    // void rollback(const rocksdb::Snapshot *snapshot);
    void rollback();
    bool commit();

private:
    rocksdb::TransactionDB *db_;
    rocksdb::TransactionDBOptions txn_db_options_;
    rocksdb::Options options_;
    rocksdb::Transaction* txn_; // Current transaction
};

#endif // KV_STORE_H