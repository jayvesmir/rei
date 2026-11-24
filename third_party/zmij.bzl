load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "zmij",
    srcs = ["zmij.cc"],
    hdrs = ["zmij.h"],
    visibility = ["//visibility:public"],
)
