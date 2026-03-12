#pragma once
#include <d3d12.h>
#include <wrl/client.h>

// D3D12 Memory Allocator (https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)
// Add D3D12MA as a submodule and include its header before this file.
#include "../../external/D3D12MA/include/D3D12MemAlloc.h"

using Microsoft::WRL::ComPtr;

// Wraps a D3D12MA::Allocation + the resource it owns.
// Movable, not copyable. Freed automatically on destruction.
struct GpuAllocation {
    D3D12MA::Allocation* allocation = nullptr;
    ComPtr<ID3D12Resource>  resource;

    GpuAllocation() = default;
    GpuAllocation(const GpuAllocation&) = delete;
    GpuAllocation& operator=(const GpuAllocation&) = delete;

    GpuAllocation(GpuAllocation&& o) noexcept
        : allocation(o.allocation), resource(std::move(o.resource))
    {
        o.allocation = nullptr;
    }

    GpuAllocation& operator=(GpuAllocation&& o) noexcept {
        Free();
        allocation = o.allocation; o.allocation = nullptr;
        resource = std::move(o.resource);
        return *this;
    }

    ~GpuAllocation() { Free(); }

    void Free() {
        resource.Reset();
        if (allocation) { allocation->Release(); allocation = nullptr; }
    }

    ID3D12Resource* Get()      const { return resource.Get(); }
    ID3D12Resource* operator->() const { return resource.Get(); }
    bool            IsValid()  const { return resource != nullptr; }
};

// ─────────────────────────────────────────────────────────────
// GpuAllocator
//   Thin wrapper around D3D12MA::Allocator.
//   Create once, pass around by pointer/reference.
// ─────────────────────────────────────────────────────────────
class GpuAllocator {
public:
    // Call after device creation.
    void Init(IDXGIAdapter* adapter, ID3D12Device* device) {
        D3D12MA::ALLOCATOR_DESC desc{};
        desc.pDevice = device;
        desc.pAdapter = adapter;
        // Allow D3D12MA to create its own heaps internally.
        desc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        HRESULT hr = D3D12MA::CreateAllocator(&desc, &m_allocator);
        if (FAILED(hr)) throw std::runtime_error("D3D12MA::CreateAllocator failed");
    }

    ~GpuAllocator() { if (m_allocator) m_allocator->Release(); }

    // ── Generic allocation helper ───────────────────────────
    // heapType: D3D12_HEAP_TYPE_DEFAULT  → GPU-only (buffers, AS, UAVs)
    //           D3D12_HEAP_TYPE_UPLOAD   → CPU-write / GPU-read (ring buffer, SBT staging)
    //           D3D12_HEAP_TYPE_READBACK → GPU-write / CPU-read (debug readbacks)
    GpuAllocation Allocate(
        const D3D12_RESOURCE_DESC& resourceDesc,
        D3D12_RESOURCE_STATES      initialState,
        D3D12_HEAP_TYPE            heapType = D3D12_HEAP_TYPE_DEFAULT)
    {
        D3D12MA::ALLOCATION_DESC allocDesc{};
        allocDesc.HeapType = heapType;

        GpuAllocation result;
        HRESULT hr = m_allocator->CreateResource(
            &allocDesc,
            &resourceDesc,
            initialState,
            nullptr,                   // no clear value for buffers
            &result.allocation,
            IID_PPV_ARGS(&result.resource));

        if (FAILED(hr)) throw std::runtime_error("D3D12MA: CreateResource failed");
        return result;
    }

    // ── Convenience: plain GPU buffer (DEFAULT heap) ────────
    GpuAllocation AllocateBuffer(
        UINT64                 sizeBytes,
        D3D12_RESOURCE_FLAGS   flags = D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATES  initialState = D3D12_RESOURCE_STATE_COMMON)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = sizeBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return Allocate(desc, initialState, D3D12_HEAP_TYPE_DEFAULT);
    }

    // ── Convenience: upload-heap buffer (CPU writable) ─────
    GpuAllocation AllocateUploadBuffer(UINT64 sizeBytes) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = sizeBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        return Allocate(desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            D3D12_HEAP_TYPE_UPLOAD);
    }

    // ── Convenience: unordered-access buffer for AS / UAV ──
    GpuAllocation AllocateUavBuffer(UINT64 sizeBytes,
        D3D12_RESOURCE_STATES initialState =
        D3D12_RESOURCE_STATE_COMMON)
    {
        return AllocateBuffer(sizeBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            initialState);
    }

    D3D12MA::Allocator* Raw() const { return m_allocator; }

private:
    D3D12MA::Allocator* m_allocator = nullptr;
};