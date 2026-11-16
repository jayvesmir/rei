CXXOPTS = select({
    "@platforms//os:windows": [
        "/std:c++23preview",
        "/utf-8",
    ],
    "//conditions:default": ["-std=c++23"],
})
