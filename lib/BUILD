load("@rules_proto//proto:defs.bzl", "proto_library")

cc_library(
    name = "common_struct",
    srcs = ["common_struct.h"],
    hdrs = ["common_struct.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "address",
    srcs = ["address.cc"],
    hdrs = ["address.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "message_handler",
    srcs = ["message_handler.h"],
    hdrs = ["message_handler.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
    ],
)

cc_library(
    name = "timer",
    srcs = ["timer.h"],
    hdrs = ["timer.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
    ],
)

cc_library(
    name = "endpoint",
    srcs = ["endpoint.cc", "udp_endpoint.cc", "signed_udp_endpoint.cc", "ipc_endpoint.cc"],
    hdrs = ["endpoint.h", "udp_endpoint.h", "signed_udp_endpoint.h", "ipc_endpoint.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        ":common_struct",
        ":message_handler",
        ":timer",
        "@com_github_enki_libev//:libev",
        "@com_github_google_glog//:glog",
        "@com_google_protobuf//:protobuf",
        "@openssl//:openssl",
    ],
)



cc_library(
    name = "utils",
    srcs = ["utils.cc"],
    hdrs = ["utils.h"],
    deps = [
        ":endpoint",
        "@com_github_gflags_gflags//:gflags",
        "@com_github_google_glog//:glog",
        "@openssl//:openssl",
    ],
    visibility = ["//visibility:public"],
)

