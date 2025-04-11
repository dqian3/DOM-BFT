cc_library(
    name = "client_class",
    srcs = ["client.cc"],
    hdrs = ["client.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:endpoint",
        "//lib:signature_provider",
        "//lib:utils",
        "//lib:common",
        "//lib:cert_collector",
        "//lib:client_record",
        "//lib:threadpool",
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
