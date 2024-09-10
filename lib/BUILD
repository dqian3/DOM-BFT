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
    name = "endpoint",
    srcs = ["transport/endpoint.cc", "transport/udp_endpoint.cc", "transport/nng_endpoint.cc"],
    hdrs = ["transport/message_handler.h", "transport/endpoint.h", "transport/udp_endpoint.h", "transport/nng_endpoint.h"],
    visibility = ["//visibility:public"],
    deps = [
        ":address",
        ":common_struct",
        ":timer",
        ":signature_provider",
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
    name = "asyncLogger",
    srcs = ["asyncLogger.cc"],
    hdrs = ["asyncLogger.h"],
    deps = [
        "@com_github_google_glog//:glog",
        ":utils",
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
    name = "application",
    hdrs = ["application.h"] + glob(["apps/*"]),
    # hdrs = ["application.h", "apps/counter.h"],
    srcs = glob(["apps/*"]),
    # srcs = ["apps/counter.cc"],
    deps = [
        "//proto:dombft_apps_cc",
        ":common_struct",
        ":protocol_config",
        "@openssl//:openssl",
        "@com_github_google_glog//:glog",
    ],
    visibility = ["//visibility:public"],
)
