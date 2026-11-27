struct varying {
    float4 clip : SV_Position;
    float3 tint;
};

[shader("vertex")]
varying vertex(float2 corner, float3 place, float3 color, uniform float scale) {
    varying result;
    result.clip = float4(corner * scale + place.xy, place.z, 1.0);
    result.tint = color;

    return result;
}

[shader("fragment")]
float4 fragment(float3 tint) : SV_Target {
    return float4(tint, 1.0);
}
