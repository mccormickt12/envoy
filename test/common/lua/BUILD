licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_test",
    "envoy_package",
)

envoy_package()

envoy_cc_test(
    name = "lua_test",
    srcs = ["lua_test.cc"],
    deps = [
        "//source/common/lua:lua_lib",
        "//test/mocks:common_lib",
        "//test/mocks/thread_local:thread_local_mocks",
        "//test/test_common:utility_lib",
    ],
)

envoy_cc_test(
    name = "wrappers_test",
    srcs = ["wrappers_test.cc"],
    deps = [
        "//source/common/buffer:buffer_lib",
        "//source/common/lua:wrappers_lib",
        "//test/test_common:lua_wrappers_lib",
        "//test/test_common:utility_lib",
    ],
)
