[shader("vertex")]
float4 numbered(uint vertex : SV_VertexID) : SV_Position {
    return float4(float(vertex), 0.0, 0.0, 1.0);
}
