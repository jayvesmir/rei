_BASELINE = ["VK_%sVERSION_1_%d" % (kind, minor) for kind in ["", "BASE_", "COMPUTE_", "GRAPHICS_"] for minor in range(5)]

_UMBRELLA_STRUCTS = ["VkPhysicalDeviceFeatures"] + ["VkPhysicalDeviceVulkan1%dFeatures" % minor for minor in range(1, 5)]

_PROPERTY_STRUCTS = {
    "VkPhysicalDeviceVulkan11Properties": "vulkan_11",
    "VkPhysicalDeviceVulkan12Properties": "vulkan_12",
}

def _attr(tag, name):
    key = " " + name + "=\""
    start = tag.find(key)
    if start < 0:
        return None
    start += len(key)
    return tag[start:tag.find("\"", start)]

def _tags(text, opening):
    return [" " + chunk[:chunk.find(">") + 1] for chunk in text.split(opening)[1:]]

def _blocks(text, opening, closing):
    blocks = []
    for chunk in text.split(opening)[1:]:
        element = opening + chunk
        head = element[:element.find(">") + 1]
        blocks.append((head[head.find(" "):] if " " in head else "", chunk[:chunk.find(closing)]))
    return blocks

def _for_vulkan(tag):
    for value in [_attr(tag, "api"), _attr(tag, "supported")]:
        if value != None and "vulkan" not in value.split(","):
            return False
    return _attr(tag, "provisional") != "true" and _attr(tag, "platform") == None

def _sections(xml):
    sections = []
    for is_core, opening, closing in [(True, "<feature api=", "</feature>"), (False, "<extension ", "</extension>")]:
        for tag, body in _blocks(xml, opening, closing):
            if _for_vulkan(tag):
                sections.append((is_core, tag, body))
    return sections

def _vendors(xml):
    return [_attr(tag, "name") for tag in _tags(xml.split("<tags")[1].split("</tags>")[0], "<tag ")]

def _snake(name):
    out = ""
    for i in range(len(name)):
        char = name[i]
        previous = name[i - 1] if i > 0 else ""
        following = name[i + 1] if i + 1 < len(name) else ""
        if char.isupper() and i > 0:
            if previous.islower() or previous.isdigit() or (previous.isupper() and following.islower()):
                out += "_"
        out += char.lower()
    return out

def _formats(xml, sections, vendors):
    names = []
    for tag in _tags(xml.split("<enums name=\"VkFormat\"")[1].split("</enums>")[0], "<enum "):
        if _attr(tag, "alias") == None:
            names.append(_attr(tag, "name"))

    for _, _, body in sections:
        for require, require_body in _blocks(body, "<require", "</require>"):
            if not _for_vulkan(require):
                continue
            for entry in _tags(require_body, "<enum "):
                name = _attr(entry, "name")
                if _attr(entry, "extends") == "VkFormat" and _attr(entry, "alias") == None and name not in names:
                    names.append(name)

    formats = []
    taken = {}
    for name in names:
        enumerator = name[len("VK_FORMAT_"):]
        for vendor in vendors:
            if enumerator.endswith("_" + vendor):
                enumerator = enumerator[:-len(vendor) - 1]
                break
        enumerator = enumerator.lower()
        if enumerator in taken:
            fail("vk_vocabulary: %s and %s would both be format::%s" % (taken[enumerator], name, enumerator))
        taken[enumerator] = name
        formats.append((enumerator, name))
    return formats

def _format_layouts(xml, formats):
    enumerators = {name: enumerator for enumerator, name in formats}
    layouts = {}
    for chunk in xml.split("<formats>")[1].split("</formats>")[0].split("<format ")[1:]:
        tag = " " + chunk[:chunk.find(">") + 1]
        name = _attr(tag, "name")
        if name not in enumerators:
            continue

        body = chunk[:chunk.find("</format>")]
        components = [_attr(" " + component, "name") for component in body.split("<component ")[1:]]
        numeric = _attr(" " + body.split("<component ")[1], "numericFormat")
        aspects = []
        if "D" in components:
            aspects.append("VK_IMAGE_ASPECT_DEPTH_BIT")
        if "S" in components:
            aspects.append("VK_IMAGE_ASPECT_STENCIL_BIT")

        layouts[enumerators[name]] = struct(
            block_size = _attr(tag, "blockSize"),
            block_extent = (_attr(tag, "blockExtent") or "1,1,1").replace(",", ", "),
            aspects = " | ".join(aspects) if aspects else "VK_IMAGE_ASPECT_COLOR_BIT",
            planar = "<plane " in body,
            numeric = "uint" if numeric == "UINT" else "sint" if numeric == "SINT" else "floating",
        )

    for enumerator, name in formats:
        if enumerator not in layouts and enumerator != "undefined":
            fail("vk_vocabulary: %s has no entry in the registry's <formats>" % name)
    return layouts

def _closing_line(text, closing):
    position = 0
    for _ in range(len(text)):
        position = text.find(closing, position)
        if position < 0 or text[text.rfind("\n", 0, position) + 1:position].strip() == "":
            return position
        position += len(closing)
    return -1

def _feature_structs(xml):
    structs = {}
    aliases = {}
    for chunk in xml.split("<type category=\"struct\"")[1:]:
        tag = " " + chunk[:chunk.find(">") + 1]
        name = _attr(tag, "name")
        if _attr(tag, "alias") != None:
            aliases[name] = _attr(tag, "alias")
            continue

        extends = (_attr(tag, "structextends") or "").split(",")
        if name != "VkPhysicalDeviceFeatures" and "VkPhysicalDeviceFeatures2" not in extends:
            continue

        structure = "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2"
        members = []
        for member in chunk[:_closing_line(chunk, "</type>")].split("<member")[1:]:
            if not _for_vulkan(" " + member[:member.find(">") + 1]):
                continue
            member_name = member.split("<name>")[1].split("</name>")[0]
            if member_name == "sType":
                structure = _attr(" " + member, "values")
            elif "<type>VkBool32</type>" in member:
                members.append(member_name)
        structs[name] = struct(structure = structure, members = members)

    return structs, {name: target for name, target in aliases.items() if target in structs}

def _owners(sections):
    owners = {}
    for is_core, tag, body in sections:
        for entry in _tags(body, "<type "):
            name = _attr(entry, "name")
            if name not in owners:
                owners[name] = struct(name = _attr(tag, "name"), is_core = is_core)
    return owners

def _minimal(alternatives):
    return [
        names
        for names in alternatives
        if not [other for other in alternatives if other != names and not [name for name in other if name not in names]]
    ]

def _any_of(left, right):
    return _minimal(left + [names for names in right if names not in left])

def _all_of(left, right):
    combined = []
    for first in left:
        for second in right:
            names = sorted({name: True for name in first + second}.keys())
            if names not in combined:
                combined.append(names)
    return _minimal(combined)

def _alternatives(expression, satisfied):
    tokens = []
    name = ""
    for char in expression.elems():
        if char in "()+,":
            tokens += [name, char] if name else [char]
            name = ""
        else:
            name += char
    if name:
        tokens.append(name)

    frames = [[[[]], "+"]]
    for token in tokens:
        if token == "(":
            frames.append([[[]], "+"])
        elif token == "+" or token == ",":
            frames[-1][1] = token
        else:
            value = frames.pop()[0] if token == ")" else ([[]] if token in satisfied else [[token]])
            frame = frames[-1]
            frame[0] = _all_of(frame[0], value) if frame[1] == "+" else _any_of(frame[0], value)
    return frames[0][0]

def _extensions(sections):
    tags = {_attr(tag, "name"): tag for is_core, tag, _ in sections if not is_core}
    satisfied = _BASELINE + [name for name, tag in tags.items() if _attr(tag, "promotedto") in _BASELINE]

    return {
        name: struct(
            is_instance = _attr(tag, "type") == "instance",
            alternatives = _alternatives(_attr(tag, "depends") or "", satisfied),
        )
        for name, tag in tags.items()
    }

def _requirements(extensions, owner):
    needed = [owner]
    for i in range(len(extensions)):
        if i == len(needed):
            break

        alternatives = extensions[needed[i]].alternatives
        if len(alternatives) != 1:
            fail("vk_vocabulary: %s can be satisfied in %d ways, which this rule can't express: %s" %
                 (needed[i], len(alternatives), alternatives))
        for name in alternatives[0]:
            if name not in extensions:
                fail("vk_vocabulary: %s needs %s, which isn't a Vulkan extension rei can enable" % (needed[i], name))
            if name not in needed:
                needed.append(name)

    return [(name, extensions[name].is_instance) for name in needed]

def _features(xml, sections, vendors):
    structs, struct_aliases = _feature_structs(xml)
    owners = _owners(sections)
    extensions = _extensions(sections)

    owned = [name for name in structs if name in owners]
    order = _UMBRELLA_STRUCTS + sorted([name for name in owned if name not in _UMBRELLA_STRUCTS])

    features = []
    keys = {}
    requirements = {}
    core = {}
    by_member = {}

    def suffix(extension):
        vendor = extension.split("_")[1]
        if vendor not in vendors:
            fail("vk_vocabulary: %s has no vendor tag" % extension)
        return "_" + vendor.lower()

    def add(key, struct_name, member, extension):
        entry = (struct_name, member, extension)
        if keys.get(key, entry) != entry:
            fail("vk_vocabulary: %s.%s and %s.%s would both be feature::%s" %
                 (keys[key][0], keys[key][1], struct_name, member, key))
        if key in keys:
            return

        keys[key] = entry
        if extension and extension not in requirements:
            requirements[extension] = _requirements(extensions, extension)
        features.append(struct(
            name = key,
            owner = struct_name,
            member = member,
            structure = structs[struct_name].structure,
            extension = extension,
        ))

    for name in order:
        owner = owners[name]
        for member in structs[name].members:
            if not owner.is_core:
                add(_snake(member) + suffix(owner.name), name, member, owner.name)
                by_member[(name, member)] = _snake(member) + suffix(owner.name)
            elif name in _UMBRELLA_STRUCTS:
                add(_snake(member), name, member, None)
                core[member] = _snake(member)
                by_member[(name, member)] = _snake(member)
            elif member not in core:
                fail("vk_vocabulary: core feature %s.%s is in none of %s" % (name, member, _UMBRELLA_STRUCTS))
            else:
                by_member[(name, member)] = core[member]

    aliases = {}
    for name, target in sorted(struct_aliases.items()):
        owner = owners.get(name)
        if owner == None or owner.is_core or target not in owners or owner.name == owners[target].name:
            continue

        for member in structs[target].members:
            key = _snake(member) + suffix(owner.name)
            if owners[target].is_core:
                if aliases.get(key, core[member]) != core[member] or key in keys:
                    fail("vk_vocabulary: feature::%s would name two features" % key)
                aliases[key] = core[member]
                by_member[(name, member)] = core[member]
            else:
                add(key, target, member, owner.name)
                by_member[(name, member)] = key

    return features, sorted(aliases.items()), requirements, by_member

def _capability_values(grammar):
    kinds = [kind for kind in json.decode(grammar)["operand_kinds"] if kind["kind"] == "Capability"]
    values = {}
    for enumerant in kinds[0]["enumerants"]:
        for name in [enumerant["enumerant"]] + enumerant.get("aliases", []):
            values[name] = enumerant["value"]
    return values

def _capabilities(xml, grammar, by_member):
    values = _capability_values(grammar)
    capabilities = []
    body = xml.split("<spirvcapabilities")[1].split("</spirvcapabilities>")[0]
    for tag, enables in _blocks(body, "<spirvcapability ", "</spirvcapability>"):
        name = _attr(tag, "name")
        if name not in values:
            continue

        always = False
        alternatives = []
        for enable in _tags(enables, "<enable "):
            version = _attr(enable, "version")
            struct_name = _attr(enable, "struct")
            property = _attr(enable, "property")
            extension = _attr(enable, "extension")
            if version != None:
                if version not in _BASELINE:
                    fail("vk_vocabulary: %s is allowed from %s, after the Vulkan 1.4 rei builds on" % (name, version))
                always = True
            elif struct_name != None:
                feature = by_member.get((struct_name, _attr(enable, "feature")))
                if feature != None:
                    alternatives.append(("feature", feature))
            elif property != None:
                if property not in _PROPERTY_STRUCTS:
                    fail("vk_vocabulary: %s depends on %s, which device::state doesn't read" % (name, property))
                member = _attr(enable, "member")
                value = _attr(enable, "value")
                described = member if value == "VK_TRUE" else "%s in %s" % (value, member)
                alternatives.append(("property", (described, _PROPERTY_STRUCTS[property], property, member, value)))
            elif extension != None:
                alternatives.append(("extension", extension))
            else:
                fail("vk_vocabulary: %s has an enable this rule doesn't know: %s" % (name, enable))

        if not always:
            unique = []
            for alternative in alternatives:
                if alternative not in unique:
                    unique.append(alternative)
            capabilities.append(struct(name = name, value = values[name], alternatives = unique))

    return sorted(capabilities, key = lambda capability: capability.value)

_HEADER = "// Generated by rei/gpu/vk/vocabulary.bzl from the Vulkan registry. Do not edit.\n\n"

def _format_header(formats):
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "#include <vulkan/vulkan_core.h>",
        "",
        "namespace rei::gpu::vk {",
        "    enum class format : std::int32_t {",
    ]
    lines += ["        %s = %s," % format for format in formats]
    lines += ["    };", "} // namespace rei::gpu::vk", ""]
    return _HEADER + "\n".join(lines)

def _format_table(formats, layouts):
    lines = [
        "#include <vulkan/vulkan_core.h>",
        "",
        "#include \"rei/gpu/vk/detail/formats.hpp\"",
        "#include \"rei/gpu/vk/format.hpp\"",
        "",
        "namespace rei::gpu::vk::detail {",
        "    auto info_of(vk::format format) noexcept -> detail::format_info {",
        "        switch (format) {",
    ]
    for enumerator, _ in formats:
        layout = layouts.get(enumerator)
        lines.append("        case vk::format::%s:" % enumerator)
        if layout == None:
            lines.append("            return {\"%s\", 0, {1, 1, 1}, 0, false, detail::numeric::floating};" % enumerator)
        else:
            lines.append("            return {\"%s\", %s, {%s}, %s, %s, detail::numeric::%s};" % (
                enumerator,
                layout.block_size,
                layout.block_extent,
                layout.aspects,
                "true" if layout.planar else "false",
                layout.numeric,
            ))
    lines += [
        "        }",
        "",
        "        return {\"unknown\", 0, {1, 1, 1}, 0, false, detail::numeric::floating};",
        "    }",
        "} // namespace rei::gpu::vk::detail",
        "",
    ]
    return _HEADER + "\n".join(lines)

def _feature_header(features, aliases):
    lines = [
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace rei::gpu::vk {",
        "    enum class feature : std::uint16_t {",
    ]
    lines += ["        %s = %d," % (feature.name, index) for index, feature in enumerate(features)]
    lines += ["        %s = %s," % alias for alias in aliases]
    lines += ["    };", "} // namespace rei::gpu::vk", ""]
    return _HEADER + "\n".join(lines)

def _feature_table(features, aliases, requirements):
    lines = [
        "#include <array>",
        "#include <cstddef>",
        "#include <span>",
        "",
        "#include <vulkan/vulkan_core.h>",
        "",
        "#include \"rei/gpu/vk/detail/features.hpp\"",
        "#include \"rei/gpu/vk/feature.hpp\"",
        "",
        "namespace rei::gpu::vk::detail {",
        "    namespace {",
    ]

    for extension, needed in sorted(requirements.items()):
        lines.append("        constexpr std::array<detail::extension_requirement, %d> %s{{" %
                     (len(needed), extension.lower()))
        lines += ["            {\"%s\", %s}," % (name, "true" if is_instance else "false") for name, is_instance in needed]
        lines += ["        }};", ""]

    lines.append("        constexpr std::array<detail::feature_info, %d> feature_data{{" % len(features))
    for feature in features:
        if feature.owner == "VkPhysicalDeviceFeatures":
            size = "sizeof(VkPhysicalDeviceFeatures2)"
            offset = "offsetof(VkPhysicalDeviceFeatures2, features) + offsetof(VkPhysicalDeviceFeatures, %s)" % feature.member
        else:
            size = "sizeof(%s)" % feature.owner
            offset = "offsetof(%s, %s)" % (feature.owner, feature.member)
        lines.append("            {\"%s\", %s, %s, %s, %s}," % (
            feature.name,
            feature.structure,
            size,
            offset,
            feature.extension.lower() if feature.extension else "{}",
        ))
    lines += ["        }};", ""]

    lines.append("        constexpr std::array<detail::feature_alias, %d> alias_data{{" % len(aliases))
    lines += ["            {\"%s\", vk::feature::%s}," % alias for alias in aliases]
    lines += [
        "        }};",
        "    } // namespace",
        "",
        "    const std::span<const detail::feature_info> feature_table = feature_data;",
        "    const std::span<const detail::feature_alias> feature_aliases = alias_data;",
        "} // namespace rei::gpu::vk::detail",
        "",
    ]
    return _HEADER + "\n".join(lines)

def _capability_table(capabilities):
    lines = [
        "#include <array>",
        "#include <cstddef>",
        "#include <span>",
        "",
        "#include <vulkan/vulkan_core.h>",
        "",
        "#include \"rei/gpu/vk/detail/features.hpp\"",
        "#include \"rei/gpu/vk/feature.hpp\"",
        "",
        "namespace rei::gpu::vk::detail {",
        "    namespace {",
    ]

    for capability in capabilities:
        lines.append("        constexpr std::array<detail::capability_enable, %d> enables_%d{{" %
                     (len(capability.alternatives), capability.value))
        for kind, detail in capability.alternatives:
            if kind == "feature":
                lines.append("            vk::feature::%s," % detail)
            elif kind == "extension":
                lines.append("            detail::extension_enable{\"%s\"}," % detail)
            else:
                described, field, property, member, value = detail
                lines.append("            detail::property_enable{\"%s\", offsetof(detail::device_properties, %s) + offsetof(%s, %s), %s}," % (
                    described,
                    field,
                    property,
                    member,
                    value,
                ))
        lines += ["        }};", ""]

    lines.append("        constexpr std::array<detail::capability_info, %d> capability_data{{" % len(capabilities))
    lines += [
        "            {%d, \"%s\", enables_%d}," % (capability.value, capability.name, capability.value)
        for capability in capabilities
    ]
    lines += [
        "        }};",
        "    } // namespace",
        "",
        "    const std::span<const detail::capability_info> capability_table = capability_data;",
        "} // namespace rei::gpu::vk::detail",
        "",
    ]
    return _HEADER + "\n".join(lines)

def _vk_vocabulary_impl(rctx):
    xml = rctx.read(rctx.path(rctx.attr.registry))
    sections = _sections(xml)
    vendors = _vendors(xml)
    features, aliases, requirements, by_member = _features(xml, sections, vendors)
    formats = _formats(xml, sections, vendors)
    capabilities = _capabilities(xml, rctx.read(rctx.path(rctx.attr.grammar)), by_member)

    rctx.file("format.hpp", _format_header(formats))
    rctx.file("format_table.cpp", _format_table(formats, _format_layouts(xml, formats)))
    rctx.file("feature.hpp", _feature_header(features, aliases))
    rctx.file("feature_table.cpp", _feature_table(features, aliases, requirements))
    rctx.file("capability_table.cpp", _capability_table(capabilities))
    rctx.file("BUILD.bazel", 'exports_files(["capability_table.cpp", "feature.hpp", "feature_table.cpp", "format.hpp", "format_table.cpp"])\n')

vk_vocabulary = repository_rule(
    implementation = _vk_vocabulary_impl,
    attrs = {
        "registry": attr.label(default = "@vulkan_headers//:registry/vk.xml", allow_single_file = True),
        "grammar": attr.label(
            default = "@spirv_headers//:include/spirv/unified1/spirv.core.grammar.json",
            allow_single_file = True,
        ),
    },
    doc = "Generates format.hpp, format_table.cpp, feature.hpp, feature_table.cpp and capability_table.cpp for " +
          "//rei/gpu:vk from vk.xml and the SPIR-V grammar.",
)

def _vk_vocabulary_headers_impl(ctx):
    headers = []
    for source in ctx.files.srcs:
        header = ctx.actions.declare_file("vk/" + source.basename)
        ctx.actions.expand_template(template = source, output = header, substitutions = {})
        headers.append(header)

    return [DefaultInfo(files = depset(headers))]

vk_vocabulary_headers = rule(
    implementation = _vk_vocabulary_headers_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = [".hpp"], mandatory = True),
    },
    doc = "Copies generated headers into the package's vk/ output directory, so they are rei/gpu/vk/*.hpp.",
)
