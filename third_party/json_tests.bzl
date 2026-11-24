filegroup(
    name = "tests",
    srcs = glob(["*.json"]),
    visibility = ["//visibility:public"],
)

exports_files(["y_array_empty.json"])
