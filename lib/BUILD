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
    srcs = ["transport/address.cc"],
    hdrs = ["transport/address.h"],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "timer",
    srcs = ["transport/timer.h"],
    hdrs = ["transport/timer.h"],
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
    name = "cert_collector",
    srcs = ["cert_collector.cc"],
    hdrs = ["cert_collector.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        ":common_struct",
        ":utils",
        "//proto:dombft_proto_cc",
        "//proto:dombft_apps_cc",
        "@com_github_google_glog//:glog",
    ]
)


cc_library(
    name = "endpoint",
    srcs = ["transport/endpoint.cc", "transport/udp_endpoint.cc", "transport/nng_endpoint.cc", "transport/nng_endpoint_threaded.cc"],
    hdrs = ["transport/message_handler.h", "transport/endpoint.h", "transport/udp_endpoint.h", "transport/nng_endpoint.h" , "transport/nng_endpoint_threaded.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        ":common_struct",
        ":timer",
        ":signature_provider",
        ":utils",
        "@com_github_enki_libev//:libev",
        "@com_github_google_glog//:glog",
        "@com_google_protobuf//:protobuf",
        "@openssl//:openssl",
        "@nng//:libnng",
    ],
)

cc_library(
    name = "utils",
    srcs = ["utils.cc"],
    hdrs = ["utils.h"],
    deps = [
        ":common_struct",
        "@com_github_gflags_gflags//:gflags",
        "@com_github_google_glog//:glog",
        "@com_github_cameron314_concurrentqueue//:concurrentqueue",
        "@com_github_cameron314_readerwriterqueue//:readerwriterqueue",
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
        ":utils",
        "@com_github_google_glog//:glog",
        "@openssl//:openssl",
        "//proto:dombft_proto_cc",
        "//lib:application",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "fallback_utils",
    hdrs = ["fallback_utils.h"],
    srcs = ["fallback_utils.cc"],
    deps = [
        ":common_struct",
        ":protocol_config",
        ":utils",
        "@com_github_google_glog//:glog",
        "@openssl//:openssl",
        "//lib:log",
        "//proto:dombft_proto_cc",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "application",
    hdrs = ["application.h"] + glob(["apps/*.h"]),
    srcs = glob(["apps/*.cc"]),
    deps = [
        "//proto:dombft_apps_cc",
        ":common_struct",
        ":protocol_config",
        "@openssl//:openssl",
        "@com_github_google_glog//:glog",
    ],
    visibility = ["//visibility:public"],
)
