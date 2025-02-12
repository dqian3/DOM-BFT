
cc_test(
  name = "test_cert_collector",
  size = "small",
  srcs = ["test_cert_collector.cc"],
  deps = [
    "//lib:cert_collector",
    "@com_google_googletest//:gtest_main",
  ],
)

cc_test(
  name = "test_client_record",
  size = "small",
  srcs = ["test_client_record.cc"],
  deps = [
    "//lib:client_record",
    "@com_google_googletest//:gtest_main",
  ],
)


cc_test(
  name = "test_fallback_utils",
  size = "small",
  srcs = ["test_fallback_utils.cc"],
  deps = [
    "//lib:fallback_utils",
    "//lib:signature_provider",
    "@com_google_googletest//:gtest_main",
  ],
)

cc_test(
  name = "test_log",
  size = "small",
  srcs = ["test_log.cc"],
  deps = [
    "//lib:log",
    "@com_google_googletest//:gtest_main"
  ],
)
