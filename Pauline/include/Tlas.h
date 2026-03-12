#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <stdexcept>
#include "Allocator.h"
#include "Blas.h"

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// TlasInstance
//   One entry in the TLAS — a BLAS reference + a 3x4 world transform + metadata.
// ─────────────────────────────────────────────────────────────────────────────
struct TlasInstance {
    const Blas* blas = nullptr;
    float       transform[3][4];       // row-major 3x4 world matrix
    UINT        instanceID = 0;     // visible to shaders as InstanceID()
    UINT        hitGroupIndex = 0;     // SBT hit group offset
    UINT        mask = 0xFF;  // visibility mask
    bool        isOpaque = true;

    // Convenience: set identity transform
    void SetIdentity() {
        transform[0][0] = 1; transform[0][1] = 0; transform[0][2] = 0; transform[0][3] = 0;
        transform[1][0] = 0; transform[1][1] = 1; transform[1][2] = 0; transform[1][3] = 0;
        transform[2][0] = 0; transform[2][1] = 0; transform[2][2] = 1; transform[2][3] = 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TlasLod
//   A complete TLAS for one LOD level. The renderer picks which level to bind
//   based on camera distance or screen coverage.
//
//   LOD 0 = full detail   (close range)
//   LOD 1 = reduced geo   (mid range)
//   LOD 2 = impostor/low  (far range)
// ─────────────────────────────────────────────────────────────────────────────
struct TlasLod {
    GpuAllocation result;
    GpuAllocation scratch;
    GpuAllocation instanceBuffer;   // upload heap, CPU-writable each frame
    UINT          instanceCount = 0;

    bool IsBuilt() const { return result.IsValid(); }

    D3D12_GPU_VIRTUAL_ADDRESS VA() const {
        return result.Get()->GetGPUVirtualAddress();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TlasBuilder
//   Builds and updates one or more TlasLod objects.
//
//   Typical frame loop (dynamic scene):
//     builder.UpdateInstances(lod0, instances, cmdList, allocator);
//     // UAV barrier emitted internally
//     cmdList->SetComputeRootShaderResourceView(slot, lod0.VA());
//
//   Static scene (build once):
//     builder.Build(device, cmdList, allocator, instances, lod0);
//     // submit, fence, then lod0.scratch.Free() if memory is tight
// ─────────────────────────────────────────────────────────────────────────────
class TlasBuilder {
public:

    // Build a TLAS from scratch. Allocates result, scratch, and instance buffer.
    void Build(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmdList,
        GpuAllocator& allocator,
        const std::vector<TlasInstance>& instances,
        TlasLod& out,
        bool                        allowUpdate = true)   // set true for dynamic scenes
    {
        out.instanceCount = static_cast<UINT>(instances.size());

        // ── Write instance descs to upload heap ───────────────
        const UINT64 instanceBufferSize =
            sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * out.instanceCount;

        out.instanceBuffer = allocator.AllocateUploadBuffer(instanceBufferSize);
        WriteInstanceDescs(instances, out.instanceBuffer);

        // ── Prebuild ──────────────────────────────────────────
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs =
            MakeInputs(out.instanceBuffer.Get()->GetGPUVirtualAddress(),
                out.instanceCount, allowUpdate);

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

        // ── Allocate ──────────────────────────────────────────
        out.result = allocator.AllocateUavBuffer(
            info.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

        out.scratch = allocator.AllocateUavBuffer(
            info.ScratchDataSizeInBytes,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // ── Record build ──────────────────────────────────────
        RecordBuild(cmdList, inputs, out, /*source=*/0);
    }

    // Re-upload instance transforms and rebuild the TLAS in-place.
    // Cheaper than a full Build() — no reallocation, no size queries.
    // The TLAS must have been built with allowUpdate = true.
    void UpdateInstances(
        ID3D12Device5* device,
        ID3D12GraphicsCommandList4* cmdList,
        GpuAllocator& allocator,
        const std::vector<TlasInstance>& instances,
        TlasLod& lod)
    {
        if (!lod.IsBuilt())
            throw std::runtime_error("TlasBuilder::UpdateInstances: TLAS not yet built");

        lod.instanceCount = static_cast<UINT>(instances.size());
        WriteInstanceDescs(instances, lod.instanceBuffer);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs =
            MakeInputs(lod.instanceBuffer.Get()->GetGPUVirtualAddress(),
                lod.instanceCount, /*allowUpdate=*/true);
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        // Re-check scratch — update scratch may differ from build scratch.
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (!lod.scratch.IsValid() ||
            lod.scratch.Get()->GetDesc().Width < info.UpdateScratchDataSizeInBytes)
        {
            lod.scratch = allocator.AllocateUavBuffer(
                info.UpdateScratchDataSizeInBytes,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        RecordBuild(cmdList, inputs, lod, /*source=*/lod.VA());
    }

    // Helper: select the best LOD given a camera distance.
    // distances[] should be sorted ascending: e.g. { 0, 50, 200 }
    // Returns the index of the chosen TlasLod.
    static UINT SelectLod(float cameraDistance,
        const float* lodDistances,
        UINT         lodCount)
    {
        for (UINT i = lodCount - 1; i > 0; --i) {
            if (cameraDistance >= lodDistances[i]) return i;
        }
        return 0;
    }

private:

    static D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS MakeInputs(
        D3D12_GPU_VIRTUAL_ADDRESS instanceVA,
        UINT                      instanceCount,
        bool                      allowUpdate)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = instanceCount;
        inputs.InstanceDescs = instanceVA;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (allowUpdate)
            inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        return inputs;
    }

    static void WriteInstanceDescs(
        const std::vector<TlasInstance>& instances,
        GpuAllocation& instanceBuffer)
    {
        D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
        instanceBuffer.Get()->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

        for (UINT i = 0; i < instances.size(); ++i) {
            const auto& src = instances[i];
            auto& dst = mapped[i];
            memset(&dst, 0, sizeof(dst));
            memcpy(dst.Transform, src.transform, sizeof(src.transform));
            dst.InstanceID = src.instanceID;
            dst.InstanceMask = src.mask;
            dst.InstanceContributionToHitGroupIndex = src.hitGroupIndex;
            dst.Flags = src.isOpaque
                ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE
                : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
            dst.AccelerationStructure = src.blas->VA();
        }

        instanceBuffer.Get()->Unmap(0, nullptr);
    }

    static void RecordBuild(
        ID3D12GraphicsCommandList4* cmdList,
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
        TlasLod& lod,
        D3D12_GPU_VIRTUAL_ADDRESS                              sourceVA)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = inputs;
        desc.DestAccelerationStructureData = lod.result.Get()->GetGPUVirtualAddress();
        desc.ScratchAccelerationStructureData = lod.scratch.Get()->GetGPUVirtualAddress();
        desc.SourceAccelerationStructureData = sourceVA; // 0 for initial build

        cmdList->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);

        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = lod.result.Get();
        cmdList->ResourceBarrier(1, &uav);
    }
};