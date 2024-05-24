cc_library(
    name = "replica_class",
    srcs = ["replica.cc"],
    hdrs = ["replica.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:message_type",
        "//lib:protocol_config",
        "//lib:log",
        "//processes:config"
    ],
)

cc_binary(
    name = "dombft_replica",
    srcs = ["replica_run.cc"],
    deps = [
        ":replica_class",
    ],
)


