RWStructuredBuffer<float> numbers;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    numbers[thread.x] *= 2.0;
}
