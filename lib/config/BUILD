
cc_library(
    name = "process_config",
    hdrs = ["process_config.h"],
    deps =
        [
            "@com_github_jbeder_yaml_cpp//:yaml-cpp",
            "//lib:application",
        ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "config_util", 
    hdrs = ["config_util.h"],
    srcs = ["config_util.cc"],
    deps =
        [
            ":process_config",
            "@com_github_jbeder_yaml_cpp//:yaml-cpp",
            "@com_github_google_glog//:glog",
            "//lib:address",
        ],
    visibility = ["//visibility:public"],
)


cc_library(
    name = "config_manager", 
    hdrs = ["config_manager.h"],
    srcs = ["config_manager.cc"],
    deps =
        [
            ":process_config",
            "@com_github_jbeder_yaml_cpp//:yaml-cpp",
            "@com_github_google_glog//:glog",
            "//lib:address",
        ],
    visibility = ["//visibility:public"],
)

# Aggregate target for the subdirectory
cc_library(
    name = "config",
    deps = [":config_util", ":config_manager", ":process_config"],
    visibility = ["//visibility:public"],
)
