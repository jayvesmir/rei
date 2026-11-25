load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "volk",
    include_prefix = "volk",

    hdrs = ["volk.h"],
    textual_hdrs = ["volk.c"],

    defines = [
        "VOLK_NAMESPACE",
        "VOLK_NO_DEVICE_PROTOTYPES",
    ],

    deps = ["@vulkan_headers"],
    visibility = ["//visibility:public"],
)
