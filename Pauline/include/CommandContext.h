#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
// ResourceStateTracker
//   Keeps a CPU-side record of every resource's current D3D12 state so callers
//   don't have to track it manually.  All barriers are batched and flushed in a
//   single ResourceBarrier() call to minimise GPU pipeline stalls.
//
//   Usage pattern (per frame):
//     tracker.Transition(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
//     ... upload data ...
//     tracker.Transition(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
//     tracker.Flush(cmdList);      // one API call, two barriers
// ─────────────────────────────────────────────────────────────────────────────
class ResourceStateTracker {
public:
    // Register a resource with its known initial state.
    void Register(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState) {
        m_states[resource] = initialState;
    }

    // Queue a transition barrier.  No-op if already in the target state.
    void Transition(ID3D12Resource* resource,
        D3D12_RESOURCE_STATES targetState,
        UINT                  subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        auto it = m_states.find(resource);
        if (it == m_states.end())
            throw std::runtime_error("ResourceStateTracker: resource not registered");

        const D3D12_RESOURCE_STATES currentState = it->second;
        if (currentState == targetState) return;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = currentState;
        barrier.Transition.StateAfter = targetState;

        m_pending.push_back(barrier);
        it->second = targetState;   // update tracked state immediately
    }

    // Queue a UAV barrier (for back-to-back UAV writes that must not overlap).
    void UavBarrier(ID3D12Resource* resource) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        m_pending.push_back(barrier);
    }

    // Submit all pending barriers in a single call.
    void Flush(ID3D12GraphicsCommandList* cmdList) {
        if (m_pending.empty()) return;
        cmdList->ResourceBarrier(static_cast<UINT>(m_pending.size()),
            m_pending.data());
        m_pending.clear();
    }

    // Query current tracked state (useful for assertions / debug).
    D3D12_RESOURCE_STATES GetState(ID3D12Resource* resource) const {
        auto it = m_states.find(resource);
        if (it == m_states.end())
            throw std::runtime_error("ResourceStateTracker: resource not registered");
        return it->second;
    }

private:
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_states;
    std::vector<D3D12_RESOURCE_BARRIER>                        m_pending;
};


// ─────────────────────────────────────────────────────────────────────────────
// CommandContext
//   Owns one command allocator + command list pair for a given queue type.
//   Wraps ResourceStateTracker so all transition logic flows through here.
//
//   Usage:
//     ctx.Open();
//     ctx.Transition(myBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
//     ctx.FlushBarriers();
//     ... record draws / dispatches / copies ...
//     ctx.Close();
//     queue->ExecuteCommandLists(1, ctx.CommandListAddr());
//     // wait for GPU, then:
//     ctx.Reset();
// ─────────────────────────────────────────────────────────────────────────────
class CommandContext {
public:
    void Init(ID3D12Device* device,
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        HRESULT hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_allocator));
        if (FAILED(hr)) throw std::runtime_error("CommandContext: CreateCommandAllocator failed");

        hr = device->CreateCommandList(0, type, m_allocator.Get(),
            nullptr, IID_PPV_ARGS(&m_cmdList));
        if (FAILED(hr)) throw std::runtime_error("CommandContext: CreateCommandList failed");

        // Command lists start open; close immediately so Reset() is the entry point.
        m_cmdList->Close();
        m_open = false;
    }

    // Re-open for a new frame / submission.  Call after GPU has finished with
    // the previous submission (i.e. after your fence wait).
    void Reset() {
        m_allocator->Reset();
        m_cmdList->Reset(m_allocator.Get(), nullptr);
        m_open = true;
    }

    void Close() {
        FlushBarriers();
        m_cmdList->Close();
        m_open = false;
    }

    // ── State helpers (delegate to tracker) ──────────────────
    void RegisterResource(ID3D12Resource* r, D3D12_RESOURCE_STATES s) {
        m_tracker.Register(r, s);
    }

    void Transition(ID3D12Resource* r,
        D3D12_RESOURCE_STATES target,
        UINT                  sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
    {
        m_tracker.Transition(r, target, sub);
    }

    void UavBarrier(ID3D12Resource* r) { m_tracker.UavBarrier(r); }

    // Flush all pending barriers immediately (call before a draw/dispatch that
    // depends on them, or just before Close() — which calls this automatically).
    void FlushBarriers() { m_tracker.Flush(m_cmdList.Get()); }

    // ── Accessors ─────────────────────────────────────────────
    ID3D12GraphicsCommandList4* CmdList()        const { return m_cmdList.Get(); }
    ID3D12GraphicsCommandList4* operator->()     const { return m_cmdList.Get(); }
    void** CommandListAddr() {
        return reinterpret_cast<void**>(m_cmdList.GetAddressOf());
    }

private:
    ComPtr<ID3D12CommandAllocator>      m_allocator;
    ComPtr<ID3D12GraphicsCommandList4>  m_cmdList;
    ResourceStateTracker                m_tracker;
    bool                                m_open = false;
};