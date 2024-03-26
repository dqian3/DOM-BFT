cc_library(
    name = "receiver_config",
    hdrs = ["receiver_config.h"],
    deps = [
        "@com_github_jbeder_yaml_cpp//:yaml-cpp",
    ],
)

cc_library(
    name = "receiver_class",
    srcs = ["receiver.cc"],
    hdrs = ["receiver.h"],
    deps = [
        "//proto:dombft_cc_proto",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:message_type",
        ":receiver_config",
    ],
)

cc_binary(
    name = "dombft_receiver",
    srcs = ["receiver_run.cc"],
    deps = [
        ":receiver_class",
    ],
)


