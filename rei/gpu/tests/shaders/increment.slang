StructuredBuffer<float> source;
RWStructuredBuffer<float> target;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    target[thread.x] = source[thread.x] + 1.0;
}
