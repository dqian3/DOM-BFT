load("@rules_proto//proto:defs.bzl", "proto_library")


proto_library(
    name = "dombft_proto",
    srcs = ["dombft_proto.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "dombft_proto_cc",
    deps = [":dombft_proto"],
    visibility = ["//visibility:public"],
)


proto_library(
    name = "dombft_apps",
    srcs = ["dombft_apps.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "dombft_apps_cc",
    deps = [":dombft_apps"],
    visibility = ["//visibility:public"],
)