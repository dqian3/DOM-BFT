cc_library(
    name = "client_class",
    srcs = ["client.cc"],
    hdrs = ["client.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:message_type",
        "//lib:endpoint",
        "//lib:signature_provider",
        "//lib:utils",
        "//lib:protocol_config",
        "//lib:cert_collector",
        "//processes:process_config",
        "//processes:config_util",
        
    ],
)

cc_binary(
    name = "dombft_client",
    srcs = ["client_run.cc"],
    deps = [
        ":client_class",
    ],
)
