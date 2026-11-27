struct camera {
    float4x4 projection;
    float4 tint;
};

struct material_data {
    Texture2D albedo;
    SamplerState nearest;
    float4 color;
};

struct corner {
    float2 position;
};

struct instance {
    float2 offset;
    float4 shade;
};

struct varying {
    float4 clip : SV_Position;
    float4 shade;
};

[[vk::push_constant]] ConstantBuffer<camera> constants;
ParameterBlock<material_data> material;

[shader("vertex")]
varying vertex(corner at, instance of) {
    varying result;
    result.clip = mul(constants.projection, float4(at.position + of.offset, 0.0, 1.0));
    result.shade = of.shade;

    return result;
}

[shader("fragment")]
float4 fragment(varying input) : SV_Target {
    const float3 texel = material.albedo.Sample(material.nearest, float2(0.5, 0.5)).rgb;

    return float4(texel.rg * input.shade.rg, material.color.b * constants.tint.b, 1.0);
}
