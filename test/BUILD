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

