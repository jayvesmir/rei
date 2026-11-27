struct light {
    float3 direction;
    float intensity;
    float3 color;
};

struct scene_data {
    light lights[2];
    float weights[3];
    float3 tint;
    uint count;
};

ConstantBuffer<scene_data> scene;
RWStructuredBuffer<float> results;

[shader("compute")]
[numthreads(1, 1, 1)]
void gather() {
    results[0] = scene.lights[0].direction.x;
    results[1] = scene.lights[1].intensity;
    results[2] = scene.lights[1].color.z;
    results[3] = scene.weights[2];
    results[4] = scene.tint.y;
    results[5] = float(scene.count);
}
