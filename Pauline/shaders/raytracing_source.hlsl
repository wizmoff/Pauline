//
// Minimal ray tracing shader library for Pauline
// DXR shader library - compile with DXC
//

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> Output : register(u0);

cbuffer FrameConstants : register(b0)
{
    float4x4 invViewProj;
    float3 cameraPos;
    uint frameIndex;
};

struct RayPayload
{
    float4 color;
};

[shader("raygeneration")]
void RayGen()
{
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    
    float2 uv = (float2(idx) + 0.5f) / float2(dim);
    Output[idx] = float4(uv.x, uv.y, 0.5f, 1.0f);
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.color = float4(0.1f, 0.2f, 0.3f, 1.0f);
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.color = float4(attr.barycentrics.xy, 0.5f, 1.0f);
}
