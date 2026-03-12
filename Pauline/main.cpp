#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

bool checkDXR() {
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    ComPtr<ID3D12Device5> device;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device));

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts, sizeof(opts));

    return opts.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

int main() {
    if (checkDXR())
        std::cout << "DXR supported";
    else
        std::cout << "DXR not supported";
    return 0;
}