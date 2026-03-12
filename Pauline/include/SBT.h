#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <stdexcept>
#include "Allocator.h"
#include "PipelineDescriptor.h"
#include "RaytracingPipeline.h"

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// ShaderBindingTable
//
//   Lays out shader records in an upload heap buffer:
//
//   [ RayGen record  ]  — exactly one, kShaderIdentifierSize bytes
//   [ Miss record    ]  — one per miss shader (we have one)
//   [ HitGroup record]  — one per instance * hit group
//
//   Each record is:
//     [ shader identifier (32 bytes) | local root arguments (padded to 64b) ]
//
//   D3D12_DISPATCH_RAYS_DESC needs:
//     - GPU VA + stride + size for each section
//
//   Usage:
//     sbt.Build(allocator, pipeline, config, materialIDs);
//     cmdList->DispatchRays(&sbt.DispatchDesc(width, height));
// ─────────────────────────────────────────────────────────────────────────────

// Local root argument for each hit group record — just the material ID.
// Must match the local root signature in RootSignature.h (one 32-bit constant).
struct HitGroupLocalArgs {
    UINT materialID = 0;
};

class ShaderBindingTable {
public:

    // recordLocalArgSize: size of local root arguments appended after the
    // shader identifier in each hit group record. For Pauline this is 4 bytes
    // (one UINT material ID). Pass 0 if no local args.
    void Build(GpuAllocator&            allocator,
               const RaytracingPipeline& pipeline,
               const PipelineConfig&     config,
               const std::vector<HitGroupLocalArgs>& hitGroupArgs)
    {
        // ── Record sizes (must be multiples of D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT = 32) ──
        // RayGen and Miss records carry no local args in Pauline.
        m_rayGenRecordSize  = AlignUp(kShaderIdentifierSize, kRecordAlignment);
        m_missRecordSize    = AlignUp(kShaderIdentifierSize, kRecordAlignment);

        // Hit group records carry one HitGroupLocalArgs after the identifier.
        const UINT localArgSize = static_cast<UINT>(sizeof(HitGroupLocalArgs));
        m_hitGroupRecordSize    = AlignUp(kShaderIdentifierSize + localArgSize, kRecordAlignment);

        const UINT numHitGroups = static_cast<UINT>(hitGroupArgs.size());

        // Total buffer size — sections must start on
        // D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT (64-byte) boundaries.
        const UINT rayGenSection   = AlignUp(m_rayGenRecordSize,              kTableAlignment);
        const UINT missSection     = AlignUp(m_missRecordSize,                kTableAlignment);
        const UINT hitGroupSection = AlignUp(m_hitGroupRecordSize * numHitGroups, kTableAlignment);

        m_rayGenOffset   = 0;
        m_missOffset     = rayGenSection;
        m_hitGroupOffset = rayGenSection + missSection;

        const UINT64 totalSize = rayGenSection + missSection + hitGroupSection;

        m_buffer = allocator.AllocateUploadBuffer(totalSize);

        // ── Map and write ──────────────────────────────────────────────────
        uint8_t* mapped = nullptr;
        m_buffer.Get()->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

        // RayGen
        WriteRecord(mapped + m_rayGenOffset,
                    pipeline.ShaderIdentifier(config.rayGenShader),
                    nullptr, 0);

        // Miss
        WriteRecord(mapped + m_missOffset,
                    pipeline.ShaderIdentifier(config.missShader),
                    nullptr, 0);

        // Hit groups
        for (UINT i = 0; i < numHitGroups; ++i) {
            const auto& hg      = config.hitGroups[i % config.hitGroups.size()];
            const auto& args    = hitGroupArgs[i];
            uint8_t*    dst     = mapped + m_hitGroupOffset + i * m_hitGroupRecordSize;
            WriteRecord(dst, pipeline.ShaderIdentifier(hg.name),
                        &args, sizeof(args));
        }

        m_buffer.Get()->Unmap(0, nullptr);

        m_numHitGroups = numHitGroups;
    }

    // Returns a filled D3D12_DISPATCH_RAYS_DESC ready for DispatchRays().
    D3D12_DISPATCH_RAYS_DESC DispatchDesc(UINT width, UINT height, UINT depth = 1) const {
        const D3D12_GPU_VIRTUAL_ADDRESS base = m_buffer.Get()->GetGPUVirtualAddress();

        D3D12_DISPATCH_RAYS_DESC desc{};

        desc.RayGenerationShaderRecord.StartAddress = base + m_rayGenOffset;
        desc.RayGenerationShaderRecord.SizeInBytes  = m_rayGenRecordSize;

        desc.MissShaderTable.StartAddress  = base + m_missOffset;
        desc.MissShaderTable.SizeInBytes   = m_missRecordSize;
        desc.MissShaderTable.StrideInBytes = m_missRecordSize;

        desc.HitGroupTable.StartAddress  = base + m_hitGroupOffset;
        desc.HitGroupTable.SizeInBytes   = m_hitGroupRecordSize * m_numHitGroups;
        desc.HitGroupTable.StrideInBytes = m_hitGroupRecordSize;

        desc.Width  = width;
        desc.Height = height;
        desc.Depth  = depth;

        return desc;
    }

private:

    static constexpr UINT kRecordAlignment = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32
    static constexpr UINT kTableAlignment  = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64

    static UINT AlignUp(UINT value, UINT align) {
        return (value + align - 1) & ~(align - 1);
    }

    static void WriteRecord(uint8_t*    dst,
                            const void* shaderIdentifier,
                            const void* localArgs,
                            UINT        localArgsSize)
    {
        memcpy(dst, shaderIdentifier, kShaderIdentifierSize);
        if (localArgs && localArgsSize > 0)
            memcpy(dst + kShaderIdentifierSize, localArgs, localArgsSize);
    }

    GpuAllocation m_buffer;
    UINT m_rayGenRecordSize    = 0;
    UINT m_missRecordSize      = 0;
    UINT m_hitGroupRecordSize  = 0;
    UINT m_rayGenOffset        = 0;
    UINT m_missOffset          = 0;
    UINT m_hitGroupOffset      = 0;
    UINT m_numHitGroups        = 0;
};
