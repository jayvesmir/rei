struct varying {
    float4 clip : SV_Position;
    float3 tint;
};

[shader("vertex")]
varying vertex(float2 position, float3 color) {
    varying result;
    result.clip = float4(position, 0.0, 1.0);
    result.tint = color;

    return result;
}

[shader("fragment")]
float4 fragment(float3 tint) : SV_Target {
    return float4(tint, 1.0);
}
