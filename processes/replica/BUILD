cc_library(
    name = "replica_config",
    hdrs = ["replica_config.h"],
    deps = [
        "@com_github_jbeder_yaml_cpp//:yaml-cpp",
    ],
)

cc_library(
    name = "replica_class",
    srcs = ["replica.cc"],
    hdrs = ["replica.h"],
    deps = [
        "//proto:dombft_cc_proto",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:common_type",
        ":replica_config",
    ],
)

cc_binary(
    name = "dombft_replica",
    srcs = ["replica_run.cc"],
    deps = [
        ":replica_class",
    ],
)


