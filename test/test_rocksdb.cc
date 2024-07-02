#include <iostream>
#include <cassert>
#include "lib/apps/kv_rocksdb.h"
#include <glog/logging.h>

int main() {
    // Step 1: Initialize the KVStore
    std::string dbPath = "./testdb";
    KVStore kvStore(dbPath);
    LOG(INFO) << "KVStore initialized";

    // Clear any existing data for a clean test environment
    // This step is assumed and depends on your KVStore implementation

    // Step 2: Begin a Transaction
    kvStore.beginTransaction();
    LOG(INFO) << "Transaction begun";

    // Step 3: Set a Key-Value Pair within the Transaction
    std::string key = "testKey";
    std::string value = "Hello, NYU!";
    bool success = kvStore.set(key, value);
    assert(success);
    LOG(INFO) << "Key-Value Pair Set";

    // Step 4: Get the Value within the Transaction
    std::string retrievedValue;
    bool found = kvStore.get(key, &retrievedValue);
    assert(found);
    std::cout << "Retrieved Value: " << retrievedValue << std::endl;
    assert(retrievedValue == value);
    LOG(INFO) << "Value Retrieved";

    // Step 5: Rollback the Transaction
    kvStore.rollback();
    LOG(INFO) << "Transaction Rolled Back";

    // Step 6: Verify the rollback - the value should not be found
    found = kvStore.get(key, &retrievedValue);
    assert(!found);
    LOG(INFO) << "Value Not Found After Rollback";

    // Step 7: Begin a new Transaction for another test
    kvStore.beginTransaction();

    // Step 8: Set a Key-Value Pair and commit this time
    key = "testKey2";
    value = "goodbye, NYU!";
    kvStore.set(key, value);
    kvStore.commit();
    
    kvStore.beginTransaction();


    // Step 9: Verify the commit - the value should be found
    found = kvStore.get(key, &retrievedValue);
    assert(found);
    std::cout << "Retrieved Value After Commit: " << retrievedValue << std::endl;
    assert(retrievedValue == value);


    // Step 10: Delete the Key
    success = kvStore.del(key);
    assert(success);



    kvStore.commit();

    // No explicit close method is shown in the provided class interface,
    // so we'll omit Step 9 from the original script.

    return 0;
}