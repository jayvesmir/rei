RWStructuredBuffer<uint> lanes;

[shader("compute")]
[numthreads(64, 1, 1)]
void lane(uint3 thread : SV_DispatchThreadID) {
    lanes[thread.x] = WaveGetLaneIndex();
}
