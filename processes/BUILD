
cc_library(
    name = "process_config",
    hdrs = ["process_config.h"],
    deps = [
        "@com_github_jbeder_yaml_cpp//:yaml-cpp",
    ],
    visibility = ["//visibility:public"],
)


cc_library(
    name = "config_util",
    hdrs = ["config_util.h"],
    srcs = ["config_util.cc"],
    deps = [
        ":process_config",
        "@com_github_jbeder_yaml_cpp//:yaml-cpp",
        "//lib:address",
        "@com_github_google_glog//:glog",
    ],
    visibility = ["//visibility:public"],
)