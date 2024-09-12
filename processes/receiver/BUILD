cc_library(
    name = "receiver_class",
    srcs = ["receiver.cc"],
    hdrs = ["receiver.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:utils",
        "//lib:address",
        "//lib:endpoint",
        "//lib:signature_provider",
        "//lib:message_type",
        "//lib:protocol_config",
        "//processes:process_config",
        "//processes:config_util",
        "//lib:asyncLogger",
    ],
)

cc_binary(
    name = "dombft_receiver",
    srcs = ["receiver_run.cc"],
    deps = [
        ":receiver_class",
    ],
)


