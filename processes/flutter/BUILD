cc_library(
    name = "flutter_replica_class",
    srcs = ["flutter_replica.cc"],
    hdrs = ["flutter_replica.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//proto:flutter_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:ooo_rpc_endpoint",
        "//lib:common",
        "//lib:threadpool",
        "//lib:config",
        "//lib:crypto",
    ],
)

cc_binary(
    name = "flutter_replica",
    srcs = ["main.cc"],
    deps = [
        ":flutter_replica_class",
        "//lib:common",
        "//lib/config:config_manager",
        "//lib/config:process_config",
    ],
)