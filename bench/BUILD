# cc_binary(
#   name = "sandbox_nng",
#   srcs = ["sandbox_nng.cc"],
#   deps = [
#     "@nng//:libnng"
#   ],
# )


cc_binary(
    name = "sandbox_crypto",
    srcs = ["sandbox_crypto.cc"],
    deps = [
        "@openssl//:openssl",
        "//lib:signature_provider",
        "//lib:utils",
    ],
)


# cc_binary(
#     name = "sandbox_mac",
#     srcs = ["sandbox_mac.cc"],
#     deps = [
#         "@openssl//:openssl",
#     ],
# )


cc_binary(
    name = "example_nng_ep_recv",
    srcs = ["example_nng_ep_recv.cc"],
    deps = [
        "//lib:endpoint",
        "//lib:log",
        "//proto:dombft_proto_cc",
    ],
)

cc_binary(
    name = "example_nng_ep_send",
    srcs = ["example_nng_ep_send.cc"],
    deps = [
        "//lib:endpoint",
        "//proto:dombft_proto_cc",
        "//proto:dombft_apps_cc",
    ],
)

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

# cc_binary(
#     name = "example_signed_udp",
#     srcs = ["example_signed_udp.cc"],
#     deps = [
#         "//lib:endpoint",
#     ],
# )

# cc_binary(
#     name = "example_recv_udp",
#     srcs = ["example_recv_udp.cc"],
#     deps = [
#         "//lib:endpoint",
#     ],
# )



# cc_binary(
#     name = "example_tcp",
#     srcs = ["example_tcp.cc"],
#     deps = [
#         "//lib:endpoint",
#     ],
# )