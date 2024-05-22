#include <gtest/gtest.h>

#include "lib/application.h"
#include "lib/apps/kv_store.h"
#include "proto/dombft_apps.pb.h"

#include <iostream>
#include <memory>

using namespace dombft::apps;

// Demonstrate some basic assertions.
TEST(TestKVStore, TestExecute)
{
    std::unique_ptr<Application> app = std::make_unique<KVStore>();

    KVRequest req;
    std::unique_ptr<AppResponse> resp;
    KVResponse *r;
    std::string key = "test";
    std::string val = "ahhhh";

    // Test set
    req.set_msg_type(KVRequestType::SET);
    req.set_key(key);
    req.set_value(val);

    resp = app->execute(req);
    r = (KVResponse *) resp.get();
    ASSERT_TRUE(r->ok());

    // Test get of prev set
    req.set_msg_type(KVRequestType::GET);
    req.set_key(key);

    resp = app->execute(req);
    r = (KVResponse *) resp.get();
    ASSERT_TRUE(r->ok());
    ASSERT_EQ(r->value(), val);

    // Test delete
    req.set_msg_type(KVRequestType::DELETE);
    req.set_key(key);

    resp = app->execute(req);
    r = (KVResponse *) resp.get();
    ASSERT_TRUE(r->ok());

    // Test get of delete is not ok
    req.set_msg_type(KVRequestType::GET);
    req.set_key(key);

    resp = app->execute(req);
    r = (KVResponse *) resp.get();
    ASSERT_FALSE(r->ok());

}
