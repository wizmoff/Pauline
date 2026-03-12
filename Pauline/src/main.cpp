#include <windows.h>
#include "../include/pauline.h"
#include "../include/D3D12Core.h"
#include "../include/Allocator.h"
#include "../include/CommandContext.h"
#include "../include/ConstantBufferArena.h"
#include "../include/Blas.h"
#include "../include/Tlas.h"
#include "../include/RootSignature.h"
#include "../include/ShaderLibrary.h"
#include "../include/PipelineDescriptor.h"
#include "../include/RaytracingPipeline.h"
#include "../include/SBT.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static constexpr UINT  kWidth  = 1280;
static constexpr UINT  kHeight = 720;

// ── Renderer ─────────────────────────────────────────────────────────────────
struct Pauline {
    D3D12Core            core;
    GpuAllocator         allocator;
    ConstantBufferArena  cbArena;
    RootSignature        rootSig;
    ShaderLibrary        shaderLib;
    RaytracingPipeline   pipeline;
    ShaderBindingTable   sbt;

    // Scene (triangle)
    GpuAllocation  vertexBuffer;
    GpuAllocation  indexBuffer;
    Blas           blas;
    TlasLod        tlas;

    // RT output UAV
    GpuAllocation  rtOutput;
    ComPtr<ID3D12DescriptorHeap> uavHeap;

    void Init(HWND hwnd) {
        core.Init(hwnd, kWidth, kHeight);
        allocator.Init(core.Adapter(), core.Device());
        cbArena.Init(core.Device(), kFrameCount, 1024 * 1024);

        // ── Root signatures ───────────────────────────────────
        rootSig.BuildGlobal(core.Device());
        rootSig.BuildLocal(core.Device());

        // ── RTPSO ─────────────────────────────────────────────
        PipelineConfig cfg{};
        cfg.payload.maxPayloadBytes   = 16;  // float4 RayPayload = 16 bytes
        cfg.payload.maxAttributeBytes = 8;   // float2 barycentrics = 8 bytes

        // ── Shader library ────────────────────────────────────
        // TODO: compile shaders/raytracing.hlsl to DXIL using DXC
        // dxc.exe -T lib_6_3 -Fo shaders/raytracing.dxil shaders/raytracing.hlsl
        try {
            shaderLib.Load(L"shaders/raytracing.dxil");
            shaderLib.RegisterExport(L"RayGen");
            shaderLib.RegisterExport(L"Miss");
            shaderLib.RegisterExport(L"ClosestHit");

            pipeline.Build(core.Device(), rootSig, shaderLib, cfg);
        } catch (const std::exception& e) {
            MessageBoxA(nullptr, 
                ("Shader compilation needed. Please run:\n"
                "dxc.exe -T lib_6_3 -Fo shaders/raytracing.dxil shaders/raytracing.hlsl\n\n"
                "Error: " + std::string(e.what())).c_str(),
                "Shader Error", MB_OK);
            throw;
        }

        // ── RT output texture (UAV) ───────────────────────────
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width            = kWidth;
            desc.Height           = kHeight;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc       = { 1, 0 };
            desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            rtOutput = allocator.Allocate(desc,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.NumDescriptors = 1;
            heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            core.Device()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap));

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            core.Device()->CreateUnorderedAccessView(
                rtOutput.Get(), nullptr, &uavDesc,
                uavHeap->GetCPUDescriptorHandleForHeapStart());
        }

        // ── Upload triangle geometry ──────────────────────────
        UploadTriangle();

        // ── SBT ───────────────────────────────────────────────
        sbt.Build(allocator, pipeline, cfg, { { 0 } }); // one hit group, material 0
    }

    void UploadTriangle() {
        // Unit triangle in world space
        struct Vertex { float x, y, z; };
        Vertex verts[] = {
            {  0.0f,  0.5f, 0.0f },
            {  0.5f, -0.5f, 0.0f },
            { -0.5f, -0.5f, 0.0f },
        };
        uint16_t indices[] = { 0, 1, 2 };

        // Staging via upload heap then copy to DEFAULT heap
        auto vbUpload = allocator.AllocateUploadBuffer(sizeof(verts));
        auto ibUpload = allocator.AllocateUploadBuffer(sizeof(indices));

        void* mapped;
        vbUpload.Get()->Map(0, nullptr, &mapped);
        memcpy(mapped, verts, sizeof(verts)); vbUpload.Get()->Unmap(0, nullptr);

        ibUpload.Get()->Map(0, nullptr, &mapped);
        memcpy(mapped, indices, sizeof(indices)); ibUpload.Get()->Unmap(0, nullptr);

        vertexBuffer = allocator.AllocateBuffer(sizeof(verts),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        indexBuffer  = allocator.AllocateBuffer(sizeof(indices),
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

        // Record copy commands
        core.BeginFrame();
        auto* cmd = core.CommandList();

        cmd->CopyResource(vertexBuffer.Get(), vbUpload.Get());
        cmd->CopyResource(indexBuffer.Get(),  ibUpload.Get());

        // Transition to shader resource
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = { vertexBuffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        barriers[1].Transition = { indexBuffer.Get(),  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
        cmd->ResourceBarrier(2, barriers);

        // Build BLAS inline
        BlasDesc bd{};
        bd.vertexBufferVA = vertexBuffer.Get()->GetGPUVirtualAddress();
        bd.vertexCount    = 3;
        bd.vertexStride   = sizeof(Vertex);
        bd.vertexFormat   = DXGI_FORMAT_R32G32B32_FLOAT;
        bd.indexBufferVA  = indexBuffer.Get()->GetGPUVirtualAddress();
        bd.indexCount     = 3;
        bd.indexFormat    = DXGI_FORMAT_R16_UINT;

        BlasBuilder blasBuilder;
        blasBuilder.Build(core.Device(), cmd, allocator, { bd }, blas,
                          /*allowCompaction=*/false);

        // Build TLAS inline
        TlasInstance inst{};
        inst.blas = &blas;
        inst.SetIdentity();
        inst.instanceID    = 0;
        inst.hitGroupIndex = 0;

        TlasBuilder tlasBuilder;
        tlasBuilder.Build(core.Device(), cmd, allocator, { inst }, tlas,
                          /*allowUpdate=*/false);

        core.EndFrame(false);
        core.FlushGpu();
    }

    void RenderFrame() {
        core.BeginFrame();
        auto* cmd = core.CommandList();

        cbArena.BeginFrame(core.FrameIndex());

        // ── Per-frame constants (identity for now) ────────────
        struct FrameConstants {
            float invViewProj[16];
            float cameraPos[3]; float _pad;
            UINT  frameIndex;   UINT _pad2[3];
        };
        FrameConstants fc{};
        // Identity inverse view-proj — triangle is already in NDC-ish space.
        fc.invViewProj[0] = fc.invViewProj[5] = fc.invViewProj[10] = fc.invViewProj[15] = 1.0f;
        fc.cameraPos[2]   = -2.0f;
        auto cb = cbArena.Push(fc);

        // ── Bind pipeline ─────────────────────────────────────
        cmd->SetComputeRootSignature(rootSig.Global());
        cmd->SetPipelineState1(pipeline.StateObject());

        ID3D12DescriptorHeap* heaps[] = { uavHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);

        cmd->SetComputeRootConstantBufferView(RootSlot::FrameConstants, cb.gpuVA);
        cmd->SetComputeRootShaderResourceView(RootSlot::Tlas,   tlas.VA());
        cmd->SetComputeRootUnorderedAccessView(RootSlot::OutputUav,
            rtOutput.Get()->GetGPUVirtualAddress());

        // ── Dispatch rays ─────────────────────────────────────
        auto dispatchDesc = sbt.DispatchDesc(kWidth, kHeight);
        cmd->DispatchRays(&dispatchDesc);

        // ── Copy RT output → back buffer ──────────────────────
        D3D12_RESOURCE_BARRIER preCopy[2]{};
        preCopy[0].Type = preCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preCopy[0].Transition = { rtOutput.Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE };
        preCopy[1].Transition = { core.BackBuffer(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_COPY_DEST };
        cmd->ResourceBarrier(2, preCopy);

        cmd->CopyResource(core.BackBuffer(), rtOutput.Get());

        // Transition RT output back to UAV for next frame
        D3D12_RESOURCE_BARRIER postCopy{};
        postCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postCopy.Transition = { rtOutput.Get(),
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        cmd->ResourceBarrier(1, &postCopy);
        // Note: back buffer RENDER_TARGET → PRESENT transition is handled by EndFrame()

        core.EndFrame();
    }
};

// ── Win32 boilerplate ─────────────────────────────────────────────────────────
static Pauline* g_pauline = nullptr;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (g_pauline && wp != SIZE_MINIMIZED)
            g_pauline->core.Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEX wc{};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"PaulineWindow";
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(0, L"PaulineWindow", L"Pauline",
        WS_OVERLAPPEDWINDOW, 100, 100, kWidth, kHeight,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) { MessageBox(nullptr, L"CreateWindowEx failed.", L"Pauline", MB_OK); return -1; }

    Pauline pauline;
    g_pauline = &pauline;

    try {
        pauline.Init(hwnd);
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Pauline Init Failed", MB_OK);
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);

    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            pauline.RenderFrame();
        }
    }

    pauline.core.FlushGpu();
    DestroyWindow(hwnd);
    return 0;
}
