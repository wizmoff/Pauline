#pragma once
#include <d3d12.h>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PipelineDescriptor
//
//   Plain-old-data configuration structs for the Raytracing Pipeline State
//   Object (RTPSO). No D3D12 objects live here — just the numbers and names
//   that describe how the pipeline should be built.
//
//   Fill one PipelineConfig and pass it to RaytracingPipeline::Build().
//   Changing any of these values requires a full RTPSO rebuild.
// ─────────────────────────────────────────────────────────────────────────────


// ── Payload & attribute sizes ─────────────────────────────────────────────────
// These MUST match the struct sizes declared in your .hlsl files.
//
// Example HLSL:
//   struct RayPayload { float3 color; float hitT; };       // 16 bytes
//   struct Attributes  { float2 bary; };                   // 8  bytes
//
// Pauline defaults:
//   Payload  : color (float3) + hitT (float) + recursion depth (uint) = 20 bytes
//   Attribute: barycentric (float2)                                    = 8  bytes
struct PayloadConfig {
    UINT maxPayloadBytes = 20;
    UINT maxAttributeBytes = 8;   // D3D12 min is 8, max is 32
};

// ── Recursion depth ────────────────────────────────────────────────────────────
// Max recursion for TraceRay() calls. 1 = primary rays only (no secondary).
// Production tip: keep this at 2 (primary + shadow) and unroll deeper bounces
// iteratively inside the RayGen shader to avoid stack overflows on low-end HW.
struct RecursionConfig {
    UINT maxRecursionDepth = 2;
};

// ── Hit group descriptor ───────────────────────────────────────────────────────
// One hit group = one logical "material type" from the GPU's perspective.
// Closest hit is required. Any hit and intersection are optional.
struct HitGroupDesc {
    std::wstring name;                          // e.g. L"HitGroup_Opaque"
    std::wstring closestHitShader;              // must match export name in ShaderLibrary
    std::wstring anyHitShader;                  // empty = not used
    std::wstring intersectionShader;            // empty = triangle (built-in)
    D3D12_HIT_GROUP_TYPE type =
        D3D12_HIT_GROUP_TYPE_TRIANGLES;
};

// ── Full pipeline config ───────────────────────────────────────────────────────
// Everything RaytracingPipeline::Build() needs to construct the state object.
struct PipelineConfig {

    // Shader entry points — must be registered in the ShaderLibrary.
    std::wstring rayGenShader = L"RayGen";
    std::wstring missShader = L"Miss";

    // Hit groups (one per material type).
    std::vector<HitGroupDesc> hitGroups = {
        { L"HitGroup_Opaque", L"ClosestHit", L"", L"" }
    };

    PayloadConfig   payload{};
    RecursionConfig recursion{};

    // Pipeline flags.
    // SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES can improve performance when
    // you know the scene only contains one primitive type.
    D3D12_RAYTRACING_PIPELINE_FLAGS pipelineFlags =
        D3D12_RAYTRACING_PIPELINE_FLAG_NONE;
};

// ── Shader identifier size (constant across all D3D12 implementations) ─────────
static constexpr UINT kShaderIdentifierSize =
D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32 bytes