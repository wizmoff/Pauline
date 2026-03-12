#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <stdexcept>
#include "Allocator.h"
#include "CommandContext.h"

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// BlasDesc
//   Describes a single piece of geometry to bake into a BLAS.
//   Fill one per mesh / submesh, then pass the array to BlasBuilder.
// ─────────────────────────────────────────────────────────────────────────────
struct BlasDesc {
    D3D12_GPU_VIRTUAL_ADDRESS vertexBufferVA;
    UINT                      vertexCount;
    UINT                      vertexStride;       // bytes per vertex
    DXGI_FORMAT               vertexFormat;       // typically DXGI_FORMAT_R32G32B32_FLOAT

    D3D12_GPU_VIRTUAL_ADDRESS indexBufferVA;      // 0 if non-indexed
    UINT                      indexCount;
    DXGI_FORMAT               indexFormat;        // R16_UINT or R32_UINT

    bool                      isOpaque = true;    // set false for alpha-tested geo
};

// ─────────────────────────────────────────────────────────────────────────────
// Blas
//   Owns the GPU memory for one Bottom-Level Acceleration Structure.
//   After Build() the scratch buffer is released.
//   After Compact() the original uncompacted buffer is released and replaced
//   with the smaller compacted one.
// ─────────────────────────────────────────────────────────────────────────────
struct Blas {
    GpuAllocation result;          // the AS itself (DEFAULT heap, UAV)
    GpuAllocation scratch;         // temporary; freed after build
    GpuAllocation compacted;       // populated by BlasBuilder::Compact()
    UINT64        compactedSize = 0;

    bool IsBuilt()    const { return result.IsValid(); }
    bool IsCompacted()const { return compacted.IsValid(); }

    // Returns the VA of whichever buffer is active (compacted if available).
    D3D12_GPU_VIRTUAL_ADDRESS VA() const {
        if (IsCompacted()) return compacted.Get()->GetGPUVirtualAddress();
        return result.Get()->GetGPUVirtualAddress();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BlasBuilder
//   Stateless helper — create once and reuse for every BLAS you need.
//
//   Two-phase compaction workflow:
//     1.  Build(...)        — records AS build into cmdList, queues size query
//     2.  [submit + fence]  — GPU executes the build
//     3.  ResolveCompactedSizes(readbackBuffer) — CPU reads back post-build sizes
//     4.  Compact(...)      — records copy into compact buffer
//     5.  [submit + fence]  — GPU executes the copy
//     6.  blas.result freed automatically once blas.compacted is populated
// ─────────────────────────────────────────────────────────────────────────────
class BlasBuilder {
public:
    // Build a BLAS from one or more geometry descs.
    // blas.result and blas.scratch are allocated here.
    // scratch is released after Compact() or when Blas is destroyed.
    void Build(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmdList,
        GpuAllocator& allocator,
        const std::vector<BlasDesc>& descs,
        Blas& out,
        bool                       allowCompaction = true,
        bool                       preferFastBuild = false)   // true for dynamic geo
    {
        // ── Build geometry descs ──────────────────────────────
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs;
        geomDescs.reserve(descs.size());

        for (const auto& d : descs) {
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = d.isOpaque
                ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;

            g.Triangles.VertexBuffer.StartAddress = d.vertexBufferVA;
            g.Triangles.VertexBuffer.StrideInBytes = d.vertexStride;
            g.Triangles.VertexCount = d.vertexCount;
            g.Triangles.VertexFormat = d.vertexFormat;

            if (d.indexBufferVA != 0) {
                g.Triangles.IndexBuffer = d.indexBufferVA;
                g.Triangles.IndexCount = d.indexCount;
                g.Triangles.IndexFormat = d.indexFormat;
            }

            geomDescs.push_back(g);
        }

        // ── Inputs ───────────────────────────────────────────
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(geomDescs.size());
        inputs.pGeometryDescs = geomDescs.data();
        inputs.Flags = preferFastBuild
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (allowCompaction)
            inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;

        // ── Size query ───────────────────────────────────────
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

        if (info.ResultDataMaxSizeInBytes == 0)
            throw std::runtime_error("BlasBuilder: prebuild info returned 0 — check geometry descs");

        // ── Allocate result + scratch ────────────────────────
        out.result = allocator.AllocateUavBuffer(
            info.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        out.scratch = allocator.AllocateUavBuffer(
            info.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // ── Compaction size readback buffer (one UINT64) ─────
        if (allowCompaction) {
            // Allocate a query heap to record post-build compacted size.
            if (!m_postBuildQueryHeap) {
                D3D12_QUERY_HEAP_DESC qhd{};
                qhd.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP; // reused below
                // Use RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO query instead:
                // We'll use the postbuild info desc directly on the build call.
            }
            m_compactSizeReadback = allocator.AllocateUploadBuffer(sizeof(UINT64));
        }

        // ── Record build ─────────────────────────────────────
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
        buildDesc.Inputs = inputs;
        buildDesc.DestAccelerationStructureData = out.result.Get()->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData = out.scratch.Get()->GetGPUVirtualAddress();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuildDesc{};
        postbuildDesc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        postbuildDesc.DestBuffer = m_compactSizeReadback.Get()->GetGPUVirtualAddress();

        cmdList->BuildRaytracingAccelerationStructure(&buildDesc, allowCompaction ? 1 : 0,
            allowCompaction ? &postbuildDesc : nullptr);

        // UAV barrier — subsequent TLAS build must see the completed BLAS.
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = out.result.Get();
        cmdList->ResourceBarrier(1, &uav);
    }

    // Call after the build command list has been executed AND the GPU fence
    // has been signalled.  Reads the compacted size back to the CPU.
    void ResolveCompactedSize(Blas& blas) {
        if (!m_compactSizeReadback.IsValid()) return;

        UINT64* mapped = nullptr;
        m_compactSizeReadback.Get()->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        blas.compactedSize = *mapped;
        m_compactSizeReadback.Get()->Unmap(0, nullptr);
    }

    // Record a copy from the uncompacted AS to a new, smaller buffer.
    // Call after ResolveCompactedSize().
    // After the copy command list executes, blas.result can be freed —
    // call FinishCompaction() once the GPU is done.
    void Compact(
        ID3D12GraphicsCommandList4* cmdList,
        GpuAllocator& allocator,
        Blas& blas)
    {
        if (blas.compactedSize == 0)
            throw std::runtime_error("BlasBuilder::Compact: call ResolveCompactedSize first");

        blas.compacted = allocator.AllocateUavBuffer(
            blas.compactedSize,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        cmdList->CopyRaytracingAccelerationStructure(
            blas.compacted.Get()->GetGPUVirtualAddress(),
            blas.result.Get()->GetGPUVirtualAddress(),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
    }

    // Call after the compaction copy has finished on the GPU.
    // Releases the original (larger) result buffer and scratch.
    void FinishCompaction(Blas& blas) {
        blas.result.Free();
        blas.scratch.Free();
    }

    // ── Refit (for deforming meshes) ─────────────────────────
    // Updates an existing BLAS in-place when vertex positions change but
    // topology is unchanged (same index buffer, same vertex count).
    // Much cheaper than a full rebuild — typically 2–4× faster on GPU.
    // NOTE: The BLAS must have been built with ALLOW_UPDATE.
    void Refit(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmdList,
        GpuAllocator& allocator,
        const std::vector<BlasDesc>& descs,
        Blas& blas)
    {
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs;
        geomDescs.reserve(descs.size());
        for (const auto& d : descs) {
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = d.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            g.Triangles.VertexBuffer.StartAddress = d.vertexBufferVA;
            g.Triangles.VertexBuffer.StrideInBytes = d.vertexStride;
            g.Triangles.VertexCount = d.vertexCount;
            g.Triangles.VertexFormat = d.vertexFormat;
            if (d.indexBufferVA) {
                g.Triangles.IndexBuffer = d.indexBufferVA;
                g.Triangles.IndexCount = d.indexCount;
                g.Triangles.IndexFormat = d.indexFormat;
            }
            geomDescs.push_back(g);
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(geomDescs.size());
        inputs.pGeometryDescs = geomDescs.data();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

        // Re-allocate scratch if needed (update scratch is usually smaller).
        if (!blas.scratch.IsValid() ||
            blas.scratch.Get()->GetDesc().Width < info.UpdateScratchDataSizeInBytes)
        {
            blas.scratch = allocator.AllocateUavBuffer(
                info.UpdateScratchDataSizeInBytes,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC refitDesc{};
        refitDesc.Inputs = inputs;
        refitDesc.SourceAccelerationStructureData = blas.VA();   // read from self
        refitDesc.DestAccelerationStructureData = blas.VA();   // write to self
        refitDesc.ScratchAccelerationStructureData = blas.scratch.Get()->GetGPUVirtualAddress();

        cmdList->BuildRaytracingAccelerationStructure(&refitDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = blas.IsCompacted() ? blas.compacted.Get() : blas.result.Get();
        cmdList->ResourceBarrier(1, &uav);
    }

private:
    GpuAllocation m_compactSizeReadback;
    ComPtr<ID3D12QueryHeap> m_postBuildQueryHeap;
};