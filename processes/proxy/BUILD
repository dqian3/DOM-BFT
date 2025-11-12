cc_library(
    name = "proxy_class",
    srcs = ["proxy.cc"],
    hdrs = ["proxy.h", "owd_calc.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:ooo_rpc_endpoint",
        "//lib:crypto",
        "//lib:common",
        "//lib:config"
    ],
)

cc_binary(
    name = "dombft_proxy",
    srcs = ["proxy_run.cc"],
    deps = [
        ":proxy_class",
    ],
)


