load("@rules_proto//proto:defs.bzl", "proto_library")

cc_library(
    name = "common",
    srcs = ["common.h"],
    hdrs = ["common.h"],
    visibility = ["//visibility:public"],
    deps=[
        "@com_github_cameron314_concurrentqueue//:concurrentqueue",
        "@com_github_cameron314_readerwriterqueue//:readerwriterqueue",
        "@com_github_preshing_junction//:libjunction",
        "@openssl//:openssl",
    ]
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
        ":common",
        "@cryptopp//:cryptopp",
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
        ":common",
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
        ":common",
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
        ":common",
        "@com_github_gflags_gflags//:gflags",
        "@com_github_google_glog//:glog",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "log",
    hdrs = ["log.h"],
    srcs = ["log.cc"],
    deps = [
        ":common",
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
        ":common",
        ":utils",
        "@com_github_google_glog//:glog",
        "@openssl//:openssl",
        "//lib:log",
        "//proto:dombft_proto_cc",
        "//lib:client_record"
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "application",
    hdrs = ["application.h"] + glob(["apps/*.h"]),
    srcs = glob(["apps/*.cc"]),
    deps = [
        "//proto:dombft_apps_cc",
        ":common",
        "@openssl//:openssl",
        "@com_github_google_glog//:glog",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "threadpool",
    srcs = ["threadpool.cc"],
    hdrs = ["threadpool.h"],
    deps = [
        ":signature_provider",
        ":utils",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "client_record",
    hdrs = ["client_record.h"],
    srcs = ["client_record.cc"],
    deps = [
        ":utils",
        ":common",
        "//proto:dombft_proto_cc"
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "checkpoint_collector",
    hdrs = ["checkpoint_collector.h"],
    srcs = ["checkpoint_collector.cc"],
    deps = [
        ":utils",
        ":log",
        ":common",
        ":client_record",
        "//proto:dombft_proto_cc"
    ],
    visibility = ["//visibility:public"],
)
