cc_library(
    name = "proxy_config",
    hdrs = ["proxy_config.h"],
    deps = [
        "@com_github_jbeder_yaml_cpp//:yaml-cpp",
    ],
)

cc_library(
    name = "proxy_class",
    srcs = ["proxy.cc"],
    hdrs = ["proxy.h"],
    deps = [
        "//proto:dombft_cc_proto",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:common_type",
        ":proxy_config",
    ],
)

cc_binary(
    name = "dombft_proxy",
    srcs = ["proxy_run.cc"],
    deps = [
        ":proxy_class",
    ],
)


