cc_library(
    name = "client_class",
    srcs = ["client.cc"],
    hdrs = ["client.h"],
    deps = [
        "//proto:dombft_proto_cc",
        "//lib:endpoint",
        "//lib:ooo_rpc_endpoint",
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






cc_library(
    name = "ooo_bench_service_msg",
    srcs = ["ooo_bench_service_msg.cc"],
    hdrs = ["ooo_bench_service_msg.h"],
    visibility = ["//visibility:public"],
    deps= [
        "//lib/rrr:rrrLib"
    ],
)

cc_library(
    name = "ooo_bench_service",
    srcs = ["ooo_bench_service.h"],
    hdrs = ["ooo_bench_service.h"],
    visibility = ["//visibility:public"],
)



cc_library(
    name = "ooo_bench_replica",
    srcs = ["ooo_bench_replica.cc"],
    hdrs = ["ooo_bench_replica.h"],
    deps= [
        ":ooo_bench_service_msg",
        "//lib:common"
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "ooo_bench_service_implementation",
    srcs = ["ooo_bench_service_implementation.cc"],
    hdrs = ["ooo_bench_service_implementation.h"],
    deps= [
        ":ooo_bench_service",
        ":ooo_bench_service_msg",
        ":ooo_bench_replica"
    ],
    visibility = ["//visibility:public"],
)

cc_binary(
    name = "bench_server",
    srcs = ["bench_server.cc"],
    deps = [
        ":ooo_bench_service_implementation",
    ],
    linkopts = [ "-L/usr/local/lib",  "-pthread",
                "-lgflags", "-lglog"
                ],
)


cc_binary(
    name = "bench_client",
    srcs = ["bench_client.cc"],
    deps = [
        ":ooo_bench_service_implementation",
    ],
    linkopts = [ "-L/usr/local/lib",  "-pthread",
                "-lgflags", "-lglog"
                ],
)

