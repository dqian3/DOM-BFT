cc_library(
    name = "replica_class",
    srcs = ["replica.cc"],
    hdrs = ["replica.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:common",
        "//lib:log",
        "//lib:cert_collector",
        "//lib:checkpoint_collector",
        "//lib:repair_utils",
        "//lib:threadpool",
        "//processes:process_config",
        "//processes:config_util"
    ],
)


cc_library(
    name = "dummy_replica_class",
    srcs = ["dummy_replica.cc"],
    hdrs = ["dummy_replica.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:common",
        "//lib:threadpool",
        "//processes:process_config",
        "//processes:config_util"
    ],
)


cc_binary(
    name = "dombft_replica",
    srcs = ["replica_run.cc"],
    deps = [
        ":replica_class",
        ":dummy_replica_class"
    ],
)


