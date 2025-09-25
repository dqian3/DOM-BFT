cc_library(
    name = "unified_replica_class",
    srcs = ["unified_replica.cc"],
    hdrs = ["unified_replica.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:ooo_rpc_endpoint",
        "//lib:common",
        "//lib:log",
        "//lib:cert_collector",
        "//lib:checkpoint_collector",
        "//lib:repair_utils",
        "//lib:threadpool",
        "//lib:crypto",
        "//processes:process_config",
        "//processes:config_util"
    ],
)

cc_binary(
    name = "dombft_unified_replica",
    srcs = ["unified_replica_run.cc"],
    deps = [
        ":unified_replica_class",
    ],
)