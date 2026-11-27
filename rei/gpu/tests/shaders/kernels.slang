StructuredBuffer<float> source;
RWStructuredBuffer<float> target;

[shader("compute")]
[numthreads(64, 1, 1)]
void scale(uint3 cell : SV_DispatchThreadID, uniform float factor) {
    target[cell.x] = source[cell.x] * factor;
}

[shader("compute")]
[numthreads(32, 1, 1)]
void shift(uint3 cell : SV_DispatchThreadID, uniform float amount) {
    target[cell.x] += amount;
}
