cc_binary(
    name = "example_nng_ep_recv",
    srcs = ["example_nng_ep_recv.cc"],
    deps = [
        "//lib:endpoint",
    ],
)

cc_binary(
    name = "example_nng_ep_send",
    srcs = ["example_nng_ep_send.cc"],
    deps = [
        "//lib:endpoint",
    ],
)



cc_binary(
    name = "example_signed_udp",
    srcs = ["example_signed_udp.cc"],
    deps = [
        "//lib:endpoint",
    ],
)

cc_binary(
    name = "example_recv_udp",
    srcs = ["example_recv_udp.cc"],
    deps = [
        "//lib:endpoint",
    ],
)

cc_binary(
    name = "sandbox_crypto",
    srcs = ["sandbox_crypto.cc"],
    deps = [
        "@openssl//:openssl",
    ],
)


cc_binary(
    name = "sandbox_mac",
    srcs = ["sandbox_mac.cc"],
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

cc_binary(
  name = "sandbox_protobuf",
  srcs = ["sandbox_protobuf.cc"],
  deps = [
    "//proto:dombft_proto_cc"
  ],
)


cc_binary(
  name = "sandbox_nng",
  srcs = ["sandbox_nng.cc"],
  deps = [
    "@nng//:libnng"
  ],
)

