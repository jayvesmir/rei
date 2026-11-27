[[vk::image_format("rgba8")]]
RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID, uniform float4 color) {
    target[thread.xy] = color;
}
