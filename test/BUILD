cc_binary(
    name = "test_signed_udp",
    srcs = ["test_signed_udp.cc"],
    deps = [
        "//lib:endpoint",
    ],
)

cc_binary(
    name = "test_recv_udp",
    srcs = ["test_recv_udp.cc"],
    deps = [
        "//lib:endpoint",
    ],
)

cc_binary(
    name = "test_crypto",
    srcs = ["test_crypto.cc"],
    deps = [
        "@openssl//:openssl",
    ],
)


cc_binary(
    name = "test_mac",
    srcs = ["test_mac.cc"],
    deps = [
        "@openssl//:openssl",
    ],
)


cc_test(
  name = "test_log",
  size = "small",
  srcs = ["test_log.cc"],
  deps = [
    "//lib:log",
    "@com_google_googletest//:gtest_main"
  ],
)

cc_test(
  name = "test_kv_store",
  size = "small",
  srcs = ["test_kv_store.cc"],
  deps = [
    "//lib:application",
    "@com_google_googletest//:gtest_main"
  ],
)
