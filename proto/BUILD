load("@rules_proto//proto:defs.bzl", "proto_library")


proto_library(
    name = "dombft_proto",
    srcs = ["dombft_proto.proto"],
    visibility = ["//visibility:public"],
)

cc_proto_library(
    name = "dombft_cc_proto",
    deps = [":dombft_proto"],
    visibility = ["//visibility:public"],
)

