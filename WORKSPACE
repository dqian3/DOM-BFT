load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository", "new_git_repository")

http_archive(
    name = "rules_proto",
    sha256 = "e017528fd1c91c5a33f15493e3a398181a9e821a804eb7ff5acdd1d2d6c2b18d",
    strip_prefix = "rules_proto-4.0.0-3.20.0",
    urls = [
        "https://github.com/bazelbuild/rules_proto/archive/refs/tags/4.0.0-3.20.0.tar.gz",
    ],
)
load("@rules_proto//proto:repositories.bzl", "rules_proto_dependencies", "rules_proto_toolchains")
rules_proto_dependencies()
rules_proto_toolchains()

http_archive(
    name = "rules_foreign_cc",
    sha256 = "2a8000ce03dd9bb324bc9bb7f1f5d01debac406611f4d9fedd385192718804f0",
    strip_prefix = "rules_foreign_cc-60813d57a0e99be1a009c1a0e9627cdbe81fcd19",
    url = "https://github.com/bazelbuild/rules_foreign_cc/archive/60813d57a0e99be1a009c1a0e9627cdbe81fcd19.tar.gz",
)
load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")
rules_foreign_cc_dependencies()

git_repository(
    name = "com_github_jbeder_yaml_cpp",
    commit = "fcbb8193b94921e058be7b563aea053531e5b2d9",  # 19-Aug-2023
    remote = "https://github.com/jbeder/yaml-cpp.git",
    shallow_since = "1692473776 -0400",
)

new_git_repository(
    name = "com_github_cameron314_concurrentqueue",
    build_file = "//third_party/concurrentqueue:BUILD.bazel",
    commit = "6dd38b8a1dbaa7863aa907045f32308a56a6ff5d",
    shallow_since = "1686439287 -0400",
    remote = "https://github.com/cameron314/concurrentqueue.git",
)

new_git_repository(
    name = "com_github_cameron314_readerwriterqueue",
    build_file = "//third_party/readerwriterqueue:BUILD.bazel",
    commit = "8e7627d18c2108aca178888d88514179899a044f", 
    shallow_since = "1628973707 -0400",
    remote = "https://github.com/cameron314/readerwriterqueue.git",
)

new_git_repository(
    name = "com_github_preshing_junction",
    commit = "5ad3be7ce1d3f16b9f7ed6065bbfeacd2d629a08",
    shallow_since = "1518982100 -0500",
    patches = ["//third_party/junction:junction.patch"],
    patch_args = ["-p1"],
    build_file = "//third_party/junction:BUILD.bazel",
    remote = "https://github.com/preshing/junction",
)

new_git_repository(
    name = "com_github_preshing_turf",
    commit = "9ae0d4b984fa95ed5f823274b39c87ee742f6650", 
    shallow_since = "1484317994 -0500" ,
    build_file = "//third_party/turf:BUILD.bazel",
    remote = "https://github.com/preshing/turf",
)

new_git_repository(
    name = "com_github_enki_libev",
    commit = "93823e6ca699df195a6c7b8bfa6006ec40ee0003",
    shallow_since = "1463172876 -0700",
    build_file = "//third_party/libev:BUILD.bazel",
    remote = "https://github.com/enki/libev.git",
)

# Google gflags.
git_repository(
    name = "com_github_gflags_gflags",
    commit = "e171aa2d15ed9eb17054558e0b3a6a413bb01067",  # 11-Nov-2018
    remote = "https://github.com/gflags/gflags.git",
    shallow_since = "1541971260 +0000",
)

# Google glog.
new_git_repository(
    name = "com_github_google_glog",
    build_file = "//third_party/glog:BUILD.glog",
    commit = "ba8a9f6952d04d1403b97df24e6836227751454e",  # 7-May-2019
    remote = "https://github.com/google/glog.git",
    # Shallow since doesn't work here for some weird reason. See
    # https://github.com/bazelbuild/bazel/issues/10292
    # shallow_since = "1557212520 +0000",
)

# Google protobuf.
git_repository(
    name = "com_google_protobuf",
    commit = "21027a27c4c2ec1000859ccbcfff46d83b16e1ed",  # 21-Apr-2022, v3.20.1
    remote = "https://github.com/protocolbuffers/protobuf",
    shallow_since = "1650589240 +0000",
)

http_archive(
    name = "openssl",
    build_file = "//third_party/openssl:BUILD.bazel",
    sha256 = "23011a5cc78e53d0dc98dfa608c51e72bcd350aa57df74c5d5574ba4ffb62e74",
    strip_prefix = "openssl-OpenSSL_1_1_1d",
    urls = ["https://github.com/openssl/openssl/archive/OpenSSL_1_1_1d.tar.gz"],
)

http_archive(
  name = "com_google_googletest",
  urls = ["https://github.com/google/googletest/archive/5ab508a01f9eb089207ee87fd547d290da39d015.zip"],
  strip_prefix = "googletest-5ab508a01f9eb089207ee87fd547d290da39d015",
)

http_archive(
  name = "nng",
  urls = ["https://github.com/nanomsg/nng/archive/refs/tags/v1.8.0.tar.gz"],
  sha256 = "cfacfdfa35c1618a28bb940e71f774a513dcb91292999696b4346ad8bfb5baff",
  strip_prefix = "nng-1.8.0",
  build_file = "//third_party/nng:BUILD.bazel",
)
