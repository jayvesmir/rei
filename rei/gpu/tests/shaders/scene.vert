#version 460

layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 place;
layout(location = 2) in vec3 color;

layout(push_constant) uniform constants { float scale; };

layout(location = 0) out vec3 tint;

void main() {
    gl_Position = vec4(corner * scale + place.xy, place.z, 1.0);
    tint = color;
}
