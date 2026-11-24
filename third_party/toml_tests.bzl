filegroup(
    name = "tests",
    srcs = glob(["tests/**"]),
    visibility = ["//visibility:public"],
)

exports_files(["tests/files-toml-1.1.0"])
