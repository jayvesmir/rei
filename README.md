<img src="https://pbs.twimg.com/media/HUgZ9p9WwAAItax?format=jpg&name=4096x4096" height=500 align="right" alt="@ayanamiplushhh on X"/>

# Rei

This is meant to be a [boost](www.boost.org)-like monolithic library. I'm adapting stuff into it from my private libraries with nicer packaging via bazel.

## Using Rei

Until Rei is at a point where it would be actually useful in a Bzlmod registry, you can link it into your project via a [git_override](https://bazel.build/rules/lib/globals/module#git_override) in your `MODULE.bazel`.

```starlark
bazel_dep(name = "rei")

git_override(
    module_name = "rei",
    remote = "https://github.com/jayvesmir/rei.git",
    commit = "<full hash>",
)
```

You can then link Rei's targets as `@rei//<target>`.  

If you want to link all parts of Rei at once, you can use the `@rei//rei:rei` target, or refer to it via just the package name `@rei//rei`.  

For a complete list of packages please look inside the [rei](rei/) directory.

## Documentation

I don't enjoy writing documentation. The code tries its best to explain itself, and has comments where needed.  

You can use [bcc](https://github.com/kiron1/bazel-compile-commands) or any other tool to generate a compilation database for your LSP.

## AI disclosure

A large portion of the code in this repository is generated using coding agents (mainly using the [Claude Code](https://claude.com/product/claude-code) harness) pulling it from my private projects and structuring it into this repository.  

I still look at and test the code written by the agents to verify it's functionality.
