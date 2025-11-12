cc_library(
    name = "client_class",
    srcs = ["client.cc"],
    hdrs = ["client.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:endpoint",
        "//lib:ooo_rpc_endpoint",
        "//lib:crypto",
        "//lib:utils",
        "//lib:common",
        "//lib:cert_collector",
        "//lib:client_record",
        "//lib:threadpool",
        "//lib:config",
    ],
)

cc_binary(
    name = "dombft_client",
    srcs = ["client_run.cc"],
    deps = [
        ":client_class",
    ],
)




