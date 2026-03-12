#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <vector>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// RootSignature
//
//   Wraps a Global Root Signature built for bindless DXR rendering.
//   Layout (must match GlobalRootSignature in every .hlsl):
//
//   [0] Root CBV  (b0) — per-frame constants (camera, frame index, etc.)
//   [1] Root SRV  (t0) — TLAS (acceleration structure)
//   [2] Root UAV  (u0) — output UAV (the RT target)
//   [3] Descriptor Table — bindless heap:
//         SRV range (t1, unbounded) — all textures / buffers
//         UAV range (u1, unbounded) — all writable buffers
//
//   Local root signatures (per-shader hit group overrides) are built
//   separately via BuildLocal() and attached during RTPSO creation.
// ─────────────────────────────────────────────────────────────────────────────

// Slot indices — keep in sync with shader register declarations.
namespace RootSlot {
    static constexpr UINT FrameConstants = 0;   // root CBV  b0
    static constexpr UINT Tlas = 1;   // root SRV  t0
    static constexpr UINT OutputUav = 2;   // root UAV  u0
    static constexpr UINT BindlessTable = 3;   // descriptor table
}

class RootSignature {
public:

    // ── Global root signature (one per renderer, shared by all shaders) ──────
    void BuildGlobal(ID3D12Device* device) {
        // [0] Root CBV — per-frame constants
        D3D12_ROOT_PARAMETER1 params[4]{};

        params[RootSlot::FrameConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[RootSlot::FrameConstants].Descriptor.ShaderRegister = 0; // b0
        params[RootSlot::FrameConstants].Descriptor.RegisterSpace = 0;
        params[RootSlot::FrameConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // [1] Root SRV — TLAS
        params[RootSlot::Tlas].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[RootSlot::Tlas].Descriptor.ShaderRegister = 0; // t0
        params[RootSlot::Tlas].Descriptor.RegisterSpace = 0;
        params[RootSlot::Tlas].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // [2] Root UAV — output texture
        params[RootSlot::OutputUav].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[RootSlot::OutputUav].Descriptor.ShaderRegister = 0; // u0
        params[RootSlot::OutputUav].Descriptor.RegisterSpace = 0;
        params[RootSlot::OutputUav].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // [3] Bindless descriptor table — unbounded SRV + UAV ranges
        // Use registers t1+ and u1+ (t0/u0 are explicitly bound above)
        static D3D12_DESCRIPTOR_RANGE1 ranges[2]{};

        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1024;  // Bounded SRV range (can increase as needed)
        ranges[0].BaseShaderRegister = 1;        // t1, space1 (not t0!)
        ranges[0].RegisterSpace = 1;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
            | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = UINT_MAX;  // Only the last range can be unbounded
        ranges[1].BaseShaderRegister = 1;        // u1, space1 (not u0!)
        ranges[1].RegisterSpace = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE
            | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

        params[RootSlot::BindlessTable].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[RootSlot::BindlessTable].DescriptorTable.NumDescriptorRanges = 2;
        params[RootSlot::BindlessTable].DescriptorTable.pDescriptorRanges = ranges;
        params[RootSlot::BindlessTable].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static sampler — bilinear wrap (common for texture lookups in hit shaders)
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = 0; // s0
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = _countof(params);
        desc.Desc_1_1.pParameters = params;
        desc.Desc_1_1.NumStaticSamplers = 1;
        desc.Desc_1_1.pStaticSamplers = &sampler;
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // DXR: no IA flag

        Compile(device, desc, m_global);
    }

    // ── Local root signature (per hit group — carries material ID) ───────────
    // Supplies one root constant (32-bit material index) visible only to the
    // HitGroup shaders. Attach to hit group subobjects in RaytracingPipeline.
    void BuildLocal(ID3D12Device* device) {
        D3D12_ROOT_PARAMETER1 param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0; // b0, space1  (local space)
        param.Constants.RegisterSpace = 1;
        param.Constants.Num32BitValues = 1; // just the material index
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
        desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        desc.Desc_1_1.NumParameters = 1;
        desc.Desc_1_1.pParameters = &param;
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

        Compile(device, desc, m_local);
    }

    ID3D12RootSignature* Global() const { return m_global.Get(); }
    ID3D12RootSignature* Local()  const { return m_local.Get(); }

private:

    static void Compile(ID3D12Device* device,
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc,
        ComPtr<ID3D12RootSignature>& out)
    {
        ComPtr<ID3DBlob> blob, error;
        // Serialize using the versioned descriptor
        HRESULT hr = D3D12SerializeVersionedRootSignature(
            &desc, &blob, &error);

        if (FAILED(hr)) {
            std::string msg = "RootSignature: serialization failed";
            if (error) msg += std::string(
                static_cast<char*>(error->GetBufferPointer()), error->GetBufferSize());
            throw std::runtime_error(msg);
        }

        hr = device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&out));
        if (FAILED(hr)) throw std::runtime_error("RootSignature: CreateRootSignature failed");
    }

    ComPtr<ID3D12RootSignature> m_global;
    ComPtr<ID3D12RootSignature> m_local;
};