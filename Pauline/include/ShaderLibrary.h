#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <stdexcept>
#include <filesystem>

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// ShaderLibrary
//
//   Loads one or more compiled DXIL shader library blobs (.cso / .dxil) from
//   disk and exposes them as D3D12_SHADER_BYTECODE for RTPSO creation.
//
//   Compile .hlsl to a library target with DXC:
//     dxc.exe -T lib_6_3 -Fo shaders/raytracing.dxil shaders/raytracing.hlsl
//
//   Usage:
//     ShaderLibrary lib;
//     lib.Load("shaders/raytracing.dxil");
//     lib.RegisterExport(L"RayGen");
//     lib.RegisterExport(L"Miss");
//     lib.RegisterExport(L"ClosestHit");
//     // Pass lib.Bytecode() and lib.Exports() into RaytracingPipeline.
//
//   Hot-reload (dev builds):
//     lib.SetWatchPath("shaders/raytracing.dxil");
//     // Each frame call lib.PollReload() — returns true if the file changed.
//     // Caller is responsible for rebuilding the RTPSO after a true return.
// ─────────────────────────────────────────────────────────────────────────────

struct ShaderExport {
    std::wstring name;          // function name in the DXIL library
    std::wstring rename;        // optional rename (empty = use name as-is)

    const wchar_t* ExportName() const {
        return rename.empty() ? name.c_str() : rename.c_str();
    }
};

class ShaderLibrary {
public:

    // Load a compiled DXIL library blob from disk.
    void Load(const std::filesystem::path& path) {
        m_path = path;
        Reload();
    }

    // Register a shader entry point to export from this library.
    // Call before passing the library to RaytracingPipeline.
    void RegisterExport(const std::wstring& name, const std::wstring& rename = {}) {
        m_exports.push_back({ name, rename });
    }

    // ── Accessors for RTPSO subobjects ───────────────────────────────────────

    D3D12_SHADER_BYTECODE Bytecode() const {
        return { m_blob->GetBufferPointer(), m_blob->GetBufferSize() };
    }

    // Returns a filled D3D12_DXIL_LIBRARY_DESC pointing at this library's
    // exports. Pointer stability: valid until the next call to RegisterExport()
    // or Reload(). Rebuild your D3D12_STATE_OBJECT after either.
    D3D12_DXIL_LIBRARY_DESC LibraryDesc() {
        RebuildExportDescs();

        D3D12_DXIL_LIBRARY_DESC desc{};
        desc.DXILLibrary = Bytecode();
        desc.NumExports = static_cast<UINT>(m_exportDescs.size());
        desc.pExports = m_exportDescs.empty() ? nullptr : m_exportDescs.data();
        return desc;
    }

    const std::vector<ShaderExport>& Exports() const { return m_exports; }

    bool IsLoaded() const { return m_blob != nullptr; }

    // ── Hot-reload (dev builds only) ─────────────────────────────────────────

    void SetWatchPath(const std::filesystem::path& path) {
        m_path = path;
        m_lastWrite = std::filesystem::last_write_time(path);
    }

    // Returns true if the file on disk is newer than when we last loaded it.
    // Caller must rebuild the RTPSO if this returns true.
    bool PollReload() {
        if (m_path.empty()) return false;

        std::error_code ec;
        auto t = std::filesystem::last_write_time(m_path, ec);
        if (ec || t == m_lastWrite) return false;

        Reload();
        return true;
    }

private:

    void Reload() {
        if (!std::filesystem::exists(m_path))
            throw std::runtime_error("ShaderLibrary: file not found: " + m_path.string());

        std::ifstream file(m_path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("ShaderLibrary: cannot open " + m_path.string());

        const std::streamsize size = file.tellg();
        file.seekg(0);

        HRESULT hr = D3DCreateBlob(static_cast<SIZE_T>(size), &m_blob);
        if (FAILED(hr)) throw std::runtime_error("ShaderLibrary: D3DCreateBlob failed");

        file.read(static_cast<char*>(m_blob->GetBufferPointer()), size);
        if (!file) throw std::runtime_error("ShaderLibrary: read failed for " + m_path.string());

        m_lastWrite = std::filesystem::last_write_time(m_path);
        m_exportDescs.clear(); // will be rebuilt on next LibraryDesc() call
    }

    void RebuildExportDescs() {
        m_exportDescs.clear();
        m_exportDescs.reserve(m_exports.size());
        for (const auto& e : m_exports) {
            D3D12_EXPORT_DESC d{};
            d.Name = e.ExportName();
            d.ExportToRename = e.rename.empty() ? nullptr : e.name.c_str();
            d.Flags = D3D12_EXPORT_FLAG_NONE;
            m_exportDescs.push_back(d);
        }
    }

    std::filesystem::path               m_path;
    std::filesystem::file_time_type     m_lastWrite{};
    ComPtr<ID3DBlob>                    m_blob;
    std::vector<ShaderExport>           m_exports;
    std::vector<D3D12_EXPORT_DESC>      m_exportDescs;
};