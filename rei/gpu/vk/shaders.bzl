load("@rules_cc//cc:cc_library.bzl", "cc_library")

def _vk_interface_impl(ctx):
    arguments = ctx.actions.args()
    arguments.add("--namespace", ctx.attr.namespace)
    arguments.add("--output", ctx.outputs.out)
    arguments.add_all(ctx.attr.entry_points, before_each = "--entry")
    arguments.add_all(ctx.files.srcs)

    ctx.actions.run(
        executable = ctx.executable._tool,
        arguments = [arguments],
        inputs = ctx.files.srcs,
        outputs = [ctx.outputs.out],
        mnemonic = "VkInterface",
        progress_message = "Generating the shader interface %{output}",
    )

    return [DefaultInfo(files = depset([ctx.outputs.out]))]

_vk_interface = rule(
    implementation = _vk_interface_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "entry_points": attr.string_list(),
        "namespace": attr.string(mandatory = True),
        "out": attr.output(mandatory = True),
        "_tool": attr.label(default = "//rei/gpu:vk_interface", executable = True, cfg = "exec"),
    },
)

def vk_shaders(name, srcs, entry_points = [], namespace = None, out = None, **kwargs):
    """Generates a C++ header with one pipeline's shaders and their typed interface.

    The header holds the SPIR-V, a function per stage that makes its vk::shader, a struct per descriptor
    set, the push constants as a struct, a checked vertex_input() and a compute shader's local_size. The
    target is a cc_library to depend on; the header is included by its path from the workspace root.

    Args:
        name: the cc_library's name.
        srcs: SPIR-V modules. Together they hold at most one entry point per stage.
        entry_points: the entry points to use where a module has more than the pipeline's.
        namespace: the header's namespace, "shaders::<name>" by default.
        out: the header's path in the package, "<name>.hpp" by default.
        **kwargs: passed to both targets, for visibility, tags and target_compatible_with.
    """
    header = out or name + ".hpp"

    _vk_interface(
        name = name + "_interface",
        srcs = srcs,
        entry_points = entry_points,
        namespace = namespace or "shaders::" + name,
        out = header,
        **kwargs
    )

    cc_library(
        name = name,
        hdrs = [header],
        deps = [Label("//rei/gpu:vk")],
        **kwargs
    )
