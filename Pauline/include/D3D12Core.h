#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include "pauline.h"

using Microsoft::WRL::ComPtr;

static constexpr UINT kFrameCount = 2; // double buffering

// ─────────────────────────────────────────────────────────────────────────────
// D3D12Core
//
//   Owns every "foundation" D3D12 object:
//     - DXGI factory + adapter
//     - ID3D12Device5 (DXR capable)
//     - Direct command queue
//     - Swap chain (double buffered)
//     - Per-frame fence + fence values
//     - RTV descriptor heap for back buffers
//
//   Usage:
//     D3D12Core core;
//     core.Init(hwnd, width, height);
//
//     // Per frame:
//     core.BeginFrame();
//     // ... record commands ...
//     core.EndFrame();      // Execute + Present + advance fence
//     core.WaitForFrame();  // CPU stall until GPU finishes current frame
// ─────────────────────────────────────────────────────────────────────────────
class D3D12Core {
public:

    void Init(HWND hwnd, UINT width, UINT height) {
        m_width  = width;
        m_height = height;

        EnableDebugLayer();
        CreateFactory();
        PickAdapter();
        CreateDevice();
        CreateCommandQueue();
        CreateSwapChain(hwnd);
        CreateRtvHeap();
        CreateRenderTargetViews();
        CreateFence();
        CreateCommandAllocators();
        CreateCommandList();
    }

    // ── Per-frame interface ───────────────────────────────────────────────────

    // Wait for the GPU to finish the *previous* use of this frame's resources.
    void WaitForFrame() {
        const UINT64 completedValue = m_fence->GetCompletedValue();
        if (completedValue < m_fenceValues[m_frameIndex]) {
            m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    // Reset allocator + list, transition back buffer to RENDER_TARGET.
    void BeginFrame() {
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
        WaitForFrame();

        m_commandAllocators[m_frameIndex]->Reset();
        m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);
    }

    // Transition back buffer to PRESENT, execute, present, signal fence.
    void EndFrame(bool vsync = true) {
        // Transition back buffer: RENDER_TARGET → PRESENT
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = m_renderTargets[m_frameIndex].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &barrier);

        m_commandList->Close();

        ID3D12CommandList* lists[] = { m_commandList.Get() };
        m_commandQueue->ExecuteCommandLists(1, lists);

        m_swapChain->Present(vsync ? 1 : 0, 0);

        // Signal fence for this frame.
        m_fenceValues[m_frameIndex]++;
        m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_frameIndex]);
    }

    // Drain the GPU completely — call before shutdown or resource rebuild.
    void FlushGpu() {
        for (UINT i = 0; i < kFrameCount; ++i) {
            m_fenceValues[i]++;
            m_commandQueue->Signal(m_fence.Get(), m_fenceValues[i]);
            if (m_fence->GetCompletedValue() < m_fenceValues[i]) {
                m_fence->SetEventOnCompletion(m_fenceValues[i], m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }
        }
    }

    void Resize(UINT width, UINT height) {
        if (width == m_width && height == m_height) return;
        FlushGpu();

        for (auto& rt : m_renderTargets) rt.Reset();

        m_swapChain->ResizeBuffers(kFrameCount, width, height,
                                   DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        m_width  = width;
        m_height = height;
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

        CreateRenderTargetViews();
    }

    ~D3D12Core() {
        FlushGpu();
        if (m_fenceEvent) CloseHandle(m_fenceEvent);
    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    IDXGIAdapter4*             Adapter()       const { return m_adapter.Get(); }
    ID3D12Device5*             Device()        const { return m_device.Get(); }
    ID3D12CommandQueue*        CommandQueue()  const { return m_commandQueue.Get(); }
    ID3D12GraphicsCommandList4* CommandList()  const { return m_commandList.Get(); }
    IDXGISwapChain4*           SwapChain()     const { return m_swapChain.Get(); }
    ID3D12Resource*            BackBuffer()    const { return m_renderTargets[m_frameIndex].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRtv()const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += m_frameIndex * m_rtvDescriptorSize;
        return handle;
    }
    UINT  FrameIndex() const { return m_frameIndex; }
    UINT  Width()      const { return m_width; }
    UINT  Height()     const { return m_height; }

private:

    void EnableDebugLayer() {
#ifdef _DEBUG
        ComPtr<ID3D12Debug1> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
        }
#endif
    }

    void CreateFactory() {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&m_factory))))
            throw std::runtime_error("D3D12Core: CreateDXGIFactory2 failed");
    }

    void PickAdapter() {
        // Pick the first adapter that supports D3D12 + DXR.
        for (UINT i = 0; ; ++i) {
            ComPtr<IDXGIAdapter1> adapter1;
            if (m_factory->EnumAdapters1(i, &adapter1) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_ADAPTER_DESC1 desc;
            adapter1->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(),
                                            D3D_FEATURE_LEVEL_12_1,
                                            __uuidof(ID3D12Device), nullptr)))
            {
                adapter1.As(&m_adapter);
                return;
            }
        }
        throw std::runtime_error("D3D12Core: no suitable DX12 adapter found");
    }

    void CreateDevice() {
        if (FAILED(D3D12CreateDevice(m_adapter.Get(),
                                     D3D_FEATURE_LEVEL_12_1,
                                     IID_PPV_ARGS(&m_device))))
            throw std::runtime_error("D3D12Core: D3D12CreateDevice failed");

        // Verify DXR support (pauline.h).
        if (!checkDXR())
            throw std::runtime_error("D3D12Core: DXR not supported on this device");

#ifdef _DEBUG
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR,   TRUE);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        }
#endif
    }

    void CreateCommandQueue() {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue))))
            throw std::runtime_error("D3D12Core: CreateCommandQueue failed");
    }

    void CreateSwapChain(HWND hwnd) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.BufferCount = kFrameCount;
        desc.Width       = m_width;
        desc.Height      = m_height;
        desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc  = { 1, 0 };

        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(m_factory->CreateSwapChainForHwnd(
                m_commandQueue.Get(), hwnd, &desc, nullptr, nullptr, &sc1)))
            throw std::runtime_error("D3D12Core: CreateSwapChainForHwnd failed");

        sc1.As(&m_swapChain);
        m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    void CreateRtvHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = kFrameCount;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap))))
            throw std::runtime_error("D3D12Core: CreateDescriptorHeap (RTV) failed");

        m_rtvDescriptorSize =
            m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    void CreateRenderTargetViews() {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT i = 0; i < kFrameCount; ++i) {
            m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
            m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
            handle.ptr += m_rtvDescriptorSize;
        }
    }

    void CreateFence() {
        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&m_fence))))
            throw std::runtime_error("D3D12Core: CreateFence failed");

        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) throw std::runtime_error("D3D12Core: CreateEvent failed");

        for (auto& v : m_fenceValues) v = 0;
    }

    void CreateCommandAllocators() {
        for (UINT i = 0; i < kFrameCount; ++i) {
            if (FAILED(m_device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&m_commandAllocators[i]))))
                throw std::runtime_error("D3D12Core: CreateCommandAllocator failed");
        }
    }

    void CreateCommandList() {
        if (FAILED(m_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_commandAllocators[0].Get(), nullptr,
                IID_PPV_ARGS(&m_commandList))))
            throw std::runtime_error("D3D12Core: CreateCommandList failed");

        m_commandList->Close(); // start closed, BeginFrame() opens it
    }

    // ── Members ───────────────────────────────────────────────────────────────
    ComPtr<IDXGIFactory6>              m_factory;
    ComPtr<IDXGIAdapter4>              m_adapter;
    ComPtr<ID3D12Device5>              m_device;
    ComPtr<ID3D12CommandQueue>         m_commandQueue;
    ComPtr<IDXGISwapChain4>            m_swapChain;
    ComPtr<ID3D12GraphicsCommandList4> m_commandList;
    ComPtr<ID3D12DescriptorHeap>       m_rtvHeap;
    ComPtr<ID3D12Resource>             m_renderTargets[kFrameCount];
    ComPtr<ID3D12CommandAllocator>     m_commandAllocators[kFrameCount];
    ComPtr<ID3D12Fence>                m_fence;
    HANDLE                             m_fenceEvent      = nullptr;
    UINT64                             m_fenceValues[kFrameCount]{};
    UINT                               m_frameIndex      = 0;
    UINT                               m_rtvDescriptorSize = 0;
    UINT                               m_width           = 0;
    UINT                               m_height          = 0;
};
