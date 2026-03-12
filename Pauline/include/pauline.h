#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

inline bool checkDXR() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<ID3D12Device5> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device))))
        return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts, sizeof(opts))))
        return false;

    return opts.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}