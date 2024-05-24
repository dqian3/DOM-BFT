load("@rules_proto//proto:defs.bzl", "proto_library")


cc_library(
    name = "protocol_config",
    srcs = ["protocol_config.h"],
    hdrs = ["protocol_config.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "common_struct",
    srcs = ["common_struct.h"],
    hdrs = ["common_struct.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "message_type",
    srcs = ["message_type.h"],
    hdrs = ["message_type.h"],
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
    srcs = ["message_handler.cc"],
    hdrs = ["message_handler.h"],
    visibility = ["//visibility:public"],
    deps = [
        "@com_github_enki_libev//:libev",
        ":address",
        ":common_struct",
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
    name = "signature_provider",
    srcs = ["signature_provider.cc"],
    hdrs = ["signature_provider.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        ":common_struct",
        "@openssl//:openssl",
        "@com_github_google_glog//:glog",
    ]
)

cc_library(
    name = "endpoint",
    srcs = ["endpoint.cc", "udp_endpoint.cc", "ipc_endpoint.cc"],
    hdrs = ["endpoint.h", "udp_endpoint.h", "ipc_endpoint.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        ":common_struct",
        ":message_handler",
        ":timer",
        ":signature_provider",
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
        "@com_github_cameron314_concurrentqueue//:concurrentqueue",
        "@com_github_preshing_junction//:libjunction",
        "@openssl//:openssl",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "log",
    hdrs = ["log.h"],
    srcs = ["log.cc"],
    deps = [

        ":common_struct",
        ":protocol_config",
        "@com_github_google_glog//:glog",
        "@openssl//:openssl",
        "//proto:dombft_proto_cc",
    ],
    visibility = ["//visibility:public"],
)


cc_library(
    name = "application",
    hdrs = ["application.h"] + glob(["apps/*"]),
    srcs = glob(["apps/*"]),
    deps = [
        "//proto:dombft_apps_cc",
        ":common_struct",
        ":protocol_config",
         "@openssl//:openssl",
    ],
    visibility = ["//visibility:public"],
)
