
cc_binary(
    name = "nng_send",
    srcs = ["nng_send.cc"],
    deps = [
        "//lib:endpoint",
        "//proto:dombft_proto_cc",
        "//proto:dombft_apps_cc",

    ],
)

cc_binary(
    name = "nng_recv",
    srcs = ["nng_recv.cc"],
    deps = [
        "//lib:endpoint",
        "//lib:log",
        "//proto:dombft_proto_cc",
    ],
)
