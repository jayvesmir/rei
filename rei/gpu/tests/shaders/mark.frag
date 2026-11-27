#version 460

layout(set = 0, binding = 0) buffer marks { float values[]; };

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 color;

void main() {
    values[0] = 42.0;
    color = vec4(uv, 0.0, 1.0);
}
