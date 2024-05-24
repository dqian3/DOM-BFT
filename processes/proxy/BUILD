cc_library(
    name = "proxy_class",
    srcs = ["proxy.cc"],
    hdrs = ["proxy.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:signature_provider",
        "//lib:message_type",
        "//lib:protocol_config",
        "//processes:config"
    ],
)

cc_binary(
    name = "dombft_proxy",
    srcs = ["proxy_run.cc"],
    deps = [
        ":proxy_class",
    ],
)


