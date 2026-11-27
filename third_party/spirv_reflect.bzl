load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "spirv_reflect",
    include_prefix = "spirv_reflect",

    srcs = ["spirv_reflect.c"],

    hdrs = [
        "include/spirv/unified1/spirv.h",
        "spirv_reflect.h",
    ],

    copts = select({
        "@platforms//os:windows": ["/w", "/DNDEBUG"],
        "//conditions:default": ["-w", "-DNDEBUG"],
    }),

    visibility = ["//visibility:public"],
)
