RWStructuredBuffer<float> marks;

struct varying {
    float4 clip : SV_Position;
    float2 uv;
};

[shader("vertex")]
varying vertex(uint index : SV_VulkanVertexID) {
    varying result;
    result.uv = float2((index << 1) & 2, index & 2);
    result.clip = float4(result.uv * 2.0 - 1.0, 0.0, 1.0);

    return result;
}

[shader("fragment")]
float4 fragment(float2 uv) : SV_Target {
    marks[0] = 42.0;

    return float4(uv, 0.0, 1.0);
}
