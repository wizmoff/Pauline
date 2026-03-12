#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <stdexcept>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// ConstantBufferArena
//
//   A single large Upload Heap divided into FrameCount equally-sized "pages".
//   Each frame writes its constant data into the current page and advances the
//   pointer.  When all FrameCount pages have been used the ring wraps.
//
//   Because each page is only written once per frame and the GPU is guaranteed
//   (by your fence) to have finished reading page N before the CPU writes to it
//   again, there are zero synchronisation stalls.
//
//   Typical use:
//
//     // --- init ---
//     arena.Init(device, FrameCount, MaxConstantsPerFrame);
//
//     // --- each frame ---
//     arena.BeginFrame(frameIndex);
//
//     CameraData cam{ ... };
//     auto [gpuVA, cbvHandle] = arena.Push(cam, cbvHeap, heapOffset);
//     cmdList->SetComputeRootConstantBufferView(0, gpuVA);
//
//     arena.EndFrame();   // optional – just advances internal bookkeeping
//
//   Rules:
//     • Push() may be called many times per frame (each gets its own 256-byte
//       aligned slot).
//     • Struct size must be ≤ MaxConstantsPerFrame.
//     • Do NOT Push() after EndFrame() for the same frameIndex.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr UINT64 kCbvAlignment = 256; // D3D12 constant buffer alignment

inline UINT64 AlignUp(UINT64 value, UINT64 align) {
    return (value + align - 1) & ~(align - 1);
}

struct CbAllocation {
    D3D12_GPU_VIRTUAL_ADDRESS gpuVA;    // pass to SetComputeRootConstantBufferView
    UINT64                    offsetBytes; // byte offset within the upload heap
    UINT64                    sizeBytes;   // aligned size actually consumed
};

class ConstantBufferArena {
public:
    // device          – your ID3D12Device
    // frameCount      – must match your swap-chain / fence depth (typically 2 or 3)
    // maxBytesPerFrame– upper bound on constant data per frame (e.g. 1 MB)
    void Init(ID3D12Device* device, UINT frameCount, UINT64 maxBytesPerFrame) {
        m_frameCount = frameCount;
        m_pageSize = AlignUp(maxBytesPerFrame, kCbvAlignment);
        m_totalSize = m_pageSize * frameCount;

        // One persistent Upload Heap for the entire ring.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = m_totalSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc = { 1, 0 };
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_buffer));

        if (FAILED(hr)) throw std::runtime_error("ConstantBufferArena: CreateCommittedResource failed");

        // Persistently map the entire heap. Safe for Upload heaps — the CPU can
        // write any time; the GPU reads only when a command list references it.
        hr = m_buffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mapped));
        if (FAILED(hr)) throw std::runtime_error("ConstantBufferArena: Map failed");

        m_gpuBase = m_buffer->GetGPUVirtualAddress();
        m_frameIndex = 0;
        m_cursor = 0;
    }

    ~ConstantBufferArena() {
        if (m_buffer && m_mapped)
            m_buffer->Unmap(0, nullptr);
    }

    // Call at the start of each frame with the current swap-chain frame index.
    void BeginFrame(UINT frameIndex) {
        m_frameIndex = frameIndex % m_frameCount;
        m_cursor = 0;   // reset the per-frame cursor
    }

    // Write 'data' into the ring and return the GPU virtual address + offset.
    // T must be a plain struct (trivially copyable).
    template<typename T>
    CbAllocation Push(const T& data) {
        static_assert(std::is_trivially_copyable_v<T>,
            "ConstantBufferArena::Push requires a trivially copyable struct");

        const UINT64 alignedSize = AlignUp(sizeof(T), kCbvAlignment);

        if (m_cursor + alignedSize > m_pageSize)
            throw std::overflow_error("ConstantBufferArena: page overflow — increase maxBytesPerFrame");

        const UINT64 pageOffset = m_frameIndex * m_pageSize;
        const UINT64 slotOffset = pageOffset + m_cursor;

        // Write to CPU-visible mapped pointer.
        std::memcpy(m_mapped + slotOffset, &data, sizeof(T));

        CbAllocation alloc{};
        alloc.gpuVA = m_gpuBase + slotOffset;
        alloc.offsetBytes = slotOffset;
        alloc.sizeBytes = alignedSize;

        m_cursor += alignedSize;
        return alloc;
    }

    // Optional — signals end of frame pushes. Currently a no-op; exists so
    // future validation (e.g. watermark tracking) can be added without API change.
    void EndFrame() {}

    ID3D12Resource* Resource() const { return m_buffer.Get(); }

    // How many bytes have been consumed in the current frame's page.
    UINT64 BytesUsedThisFrame() const { return m_cursor; }

private:
    ComPtr<ID3D12Resource>    m_buffer;
    BYTE* m_mapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_gpuBase = 0;
    UINT64                    m_pageSize = 0;
    UINT64                    m_totalSize = 0;
    UINT64                    m_cursor = 0;   // byte offset within current page
    UINT                      m_frameCount = 0;
    UINT                      m_frameIndex = 0;
};