#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <stdexcept>
#include "RootSignature.h"
#include "ShaderLibrary.h"
#include "PipelineDescriptor.h"

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// RaytracingPipeline
//
//   Owns the ID3D12StateObject (RTPSO) and exposes shader identifiers for SBT
//   construction. Building the RTPSO can take 100–500ms on first compile —
//   do this at load time, never per-frame.
//
//   Usage:
//     RaytracingPipeline pipeline;
//     pipeline.Build(device, rootSig, shaderLib, config);
//
//     // During SBT construction:
//     void* rayGenID  = pipeline.ShaderIdentifier(config.rayGenShader);
//     void* missID    = pipeline.ShaderIdentifier(config.missShader);
//     void* hitID     = pipeline.ShaderIdentifier(L"HitGroup_Opaque");
// ─────────────────────────────────────────────────────────────────────────────
class RaytracingPipeline {
public:

    void Build(ID3D12Device5* device,
        const RootSignature& rootSig,
        ShaderLibrary& shaderLib,
        const PipelineConfig& cfg)
    {
        // Each subobject is a tagged pointer into a contiguous array.
        // We need:
        //   1  DXIL library
        //   N  hit group subobjects (one per HitGroupDesc)
        //   1  shader config (payload / attribute sizes)
        //   1  pipeline config (max recursion + flags)
        //   1  global root signature association
        //   1  local root signature association (per hit group)
        //   1  global root signature subobject
        //   1  local root signature subobject
        // Total: 7 + N (where N = hitGroups.size())

        const UINT numHitGroups = static_cast<UINT>(cfg.hitGroups.size());
        const UINT numSubobjects = 8 + numHitGroups;

        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
        subobjects.reserve(numSubobjects);

        // ── 1. DXIL library ─────────────────────────────────────────────────
        auto libDesc = shaderLib.LibraryDesc();
        {
            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
            so.pDesc = &libDesc;
            subobjects.push_back(so);
        }

        // ── 2. Hit groups ────────────────────────────────────────────────────
        std::vector<D3D12_HIT_GROUP_DESC> hitGroupDescs(numHitGroups);
        for (UINT i = 0; i < numHitGroups; ++i) {
            const auto& hg = cfg.hitGroups[i];
            auto& d = hitGroupDescs[i];
            d.HitGroupExport = hg.name.c_str();
            d.Type = hg.type;
            d.ClosestHitShaderImport = hg.closestHitShader.empty() ? nullptr : hg.closestHitShader.c_str();
            d.AnyHitShaderImport = hg.anyHitShader.empty() ? nullptr : hg.anyHitShader.c_str();
            d.IntersectionShaderImport = hg.intersectionShader.empty() ? nullptr : hg.intersectionShader.c_str();

            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
            so.pDesc = &d;
            subobjects.push_back(so);
        }

        // ── 3. Shader config (payload + attribute sizes) ─────────────────────
        D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{};
        shaderCfg.MaxPayloadSizeInBytes = cfg.payload.maxPayloadBytes;
        shaderCfg.MaxAttributeSizeInBytes = cfg.payload.maxAttributeBytes;
        {
            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
            so.pDesc = &shaderCfg;
            subobjects.push_back(so);
        }

        // ── 4. Pipeline config (max recursion depth + flags) ─────────────────
        D3D12_RAYTRACING_PIPELINE_CONFIG1 pipelineCfg{};
        pipelineCfg.MaxTraceRecursionDepth = cfg.recursion.maxRecursionDepth;
        pipelineCfg.Flags = cfg.pipelineFlags;
        {
            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
            so.pDesc = &pipelineCfg;
            subobjects.push_back(so);
        }

        // ── 5. Global root signature ─────────────────────────────────────────
        // The subobject must outlive CreateStateObject — store as member.
        m_globalRSWrapper.pGlobalRootSignature = rootSig.Global();
        {
            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
            so.pDesc = &m_globalRSWrapper;
            subobjects.push_back(so);
        }

        // ── 6. Local root signature ──────────────────────────────────────────
        m_localRSWrapper.pLocalRootSignature = rootSig.Local();
        const UINT localRSIndex = static_cast<UINT>(subobjects.size());
        {
            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
            so.pDesc = &m_localRSWrapper;
            subobjects.push_back(so);
        }

        // ── 7. Associate local root signature with all hit groups ────────────
        std::vector<const wchar_t*> hitGroupNames;
        hitGroupNames.reserve(numHitGroups);
        for (const auto& hg : cfg.hitGroups)
            hitGroupNames.push_back(hg.name.c_str());

        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION localAssoc{};
        localAssoc.pSubobjectToAssociate = &subobjects[localRSIndex];
        localAssoc.NumExports = numHitGroups;
        localAssoc.pExports = hitGroupNames.data();
        {
            D3D12_STATE_SUBOBJECT so{};
            so.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
            so.pDesc = &localAssoc;
            subobjects.push_back(so);
        }

        // ── Create state object ──────────────────────────────────────────────
        D3D12_STATE_OBJECT_DESC soDesc{};
        soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        soDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
        soDesc.pSubobjects = subobjects.data();

        ComPtr<ID3D12InfoQueue> infoQueue;
        device->QueryInterface(IID_PPV_ARGS(&infoQueue));

        HRESULT hr = device->CreateStateObject(&soDesc, IID_PPV_ARGS(&m_stateObject));
        if (FAILED(hr)) {
            std::string errorMsg = "RaytracingPipeline: CreateStateObject failed (HRESULT: 0x" 
                + std::to_string(hr) + ")";

            if (infoQueue) {
                UINT64 msgCount = infoQueue->GetNumStoredMessages();
                for (UINT64 i = 0; i < msgCount && i < 5; ++i) {
                    SIZE_T msgLen = 0;
                    infoQueue->GetMessage(i, nullptr, &msgLen);
                    if (msgLen > 0) {
                        std::vector<uint8_t> buf(msgLen);
                        D3D12_MESSAGE* pMsg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                        infoQueue->GetMessage(i, pMsg, &msgLen);
                        errorMsg += "\n  Debug: " + std::string(pMsg->pDescription);
                    }
                }
            }
            throw std::runtime_error(errorMsg);
        }

        // Cache shader identifiers for SBT construction.
        CacheIdentifiers(cfg);
    }

    // Returns a pointer to the 32-byte shader identifier for the given export
    // name. Pass this directly into your SBT records.
    const void* ShaderIdentifier(const std::wstring& exportName) const {
        auto it = m_identifiers.find(exportName);
        if (it == m_identifiers.end())
            throw std::runtime_error("RaytracingPipeline: unknown export name");
        return it->second.data();
    }

    ID3D12StateObject* StateObject()     const { return m_stateObject.Get(); }
    ID3D12StateObjectProperties* Properties()      const { return m_properties.Get(); }

private:

    void CacheIdentifiers(const PipelineConfig& cfg) {
        HRESULT hr = m_stateObject->QueryInterface(IID_PPV_ARGS(&m_properties));
        if (FAILED(hr)) throw std::runtime_error("RaytracingPipeline: QueryInterface for properties failed");

        auto cache = [&](const std::wstring& name) {
            void* id = m_properties->GetShaderIdentifier(name.c_str());
            if (!id) throw std::runtime_error("RaytracingPipeline: shader identifier not found");
            auto& slot = m_identifiers[name];
            slot.resize(kShaderIdentifierSize);
            memcpy(slot.data(), id, kShaderIdentifierSize);
            };

        cache(cfg.rayGenShader);
        cache(cfg.missShader);
        for (const auto& hg : cfg.hitGroups)
            cache(hg.name);
    }

    ComPtr<ID3D12StateObject>           m_stateObject;
    ComPtr<ID3D12StateObjectProperties> m_properties;

    // Must stay alive for the lifetime of the state object.
    D3D12_GLOBAL_ROOT_SIGNATURE m_globalRSWrapper{};
    D3D12_LOCAL_ROOT_SIGNATURE  m_localRSWrapper{};

    std::unordered_map<std::wstring, std::vector<uint8_t>> m_identifiers;
};