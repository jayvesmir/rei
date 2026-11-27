#version 460

layout(set = 0, binding = 0) uniform sampler2D scene;

layout(push_constant) uniform constants { vec4 tint; };

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 color;

void main() {
    color = texture(scene, uv) * tint;
}
