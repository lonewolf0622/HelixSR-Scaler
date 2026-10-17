#pragma once

#include <d3d12.h>
#include <array>
#include <optional>
#include <utility>

namespace HelixSR::Dx12
{
struct ResourceKeys
{
    const char* ngx;
    const char* resource;
    const char* state;
};

inline constexpr std::array<ResourceKeys, 6> kResourceKeys = { {
    { "Color", "HelixSR.Color.Resource", "HelixSR.Color.State" },
    { "Depth", "HelixSR.Depth.Resource", "HelixSR.Depth.State" },
    { "MotionVectors", "HelixSR.Motion.Resource", "HelixSR.Motion.State" },
    { "ExposureTexture", "HelixSR.Exposure.Resource", "HelixSR.Exposure.State" },
    { "DLSS.Input.Bias.Current.Color.Mask", "HelixSR.Mask.Resource", "HelixSR.Mask.State" },
    { "Output", "HelixSR.Output.Resource", "HelixSR.Output.State" },
} };

// FSR 2's state bits differ from FSR 3's. Zero is unspecified in FSR 2.
inline std::optional<D3D12_RESOURCE_STATES> Fsr2State(unsigned state)
{
    switch (state)
    {
    case 1:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case 2:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 4:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case 8:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case 6:
        return D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    default:
        return std::nullopt;
    }
}

// FSR 3 and the FidelityFX API share this state layout.
inline std::optional<D3D12_RESOURCE_STATES> Fsr3State(unsigned state)
{
    switch (state)
    {
    case 1:
    case 128:
        return D3D12_RESOURCE_STATE_COMMON;
    case 2:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case 4:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 8:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case 12:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 16:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case 32:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case 20:
        return D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case 64:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case 256:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case 512:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    default:
        return std::nullopt;
    }
}

// Keep states with their resources, per dispatch. Output scaling / RCAS can
// replace an NGX resource with a different texture before the backend runs.
template <typename Params, typename Dispatch, typename Convert>
void CaptureStates(Params* params, const Dispatch& dispatch, Convert convert)
{
    const decltype(dispatch.color)* resources[] = { &dispatch.color,    &dispatch.depth,    &dispatch.motionVectors,
                                                    &dispatch.exposure, &dispatch.reactive, &dispatch.output };
    for (unsigned i = 0; i < kResourceKeys.size(); ++i)
    {
        const auto state = convert(resources[i]->state);
        params->Set(kResourceKeys[i].resource, state ? resources[i]->resource : nullptr);
        params->Set(kResourceKeys[i].state, static_cast<unsigned>(state.value_or(D3D12_RESOURCE_STATE_COMMON)));
    }
}

template <typename Params>
std::optional<D3D12_RESOURCE_STATES> InputState(Params* params, unsigned index, ID3D12Resource* resource,
                                                std::optional<D3D12_RESOURCE_STATES> fallback,
                                                std::optional<D3D12_RESOURCE_STATES> configured = std::nullopt)
{
    ID3D12Resource* original = nullptr;
    unsigned state = 0;
    const auto& keys = kResourceKeys[index];
    if (params->Get(keys.resource, &original) == 1)
    {
        if (!original)
            return configured; // Unspecified state: do not reuse another dispatch's volatile hotfix.
        if (original != resource)
            return std::nullopt; // Scaler's scratch texture already has its required state.
        if (configured)
            return configured;
        if (params->Get(keys.state, &state) == 1)
            return static_cast<D3D12_RESOURCE_STATES>(state);
    }
    return configured ? configured : fallback;
}

// Scoped so inputs and output are restored even when evaluation fails.
// The command-list type is generic to allow a CPU-only recording test.
template <typename CommandList> class ResourceTransitions
{
    CommandList* _list;
    std::array<D3D12_RESOURCE_BARRIER, 6> _barriers {};
    unsigned _count = 0;
    bool _applied = false;

  public:
    explicit ResourceTransitions(CommandList* list) : _list(list) {}
    ResourceTransitions(const ResourceTransitions&) = delete;
    ResourceTransitions& operator=(const ResourceTransitions&) = delete;

    void Add(ID3D12Resource* resource, std::optional<D3D12_RESOURCE_STATES> before, D3D12_RESOURCE_STATES after)
    {
        if (!resource || !before || *before == after)
            return;
        for (unsigned i = 0; i < _count; ++i)
            if (_barriers[i].Transition.pResource == resource)
                return;
        auto& barrier = _barriers.at(_count++);
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = *before;
        barrier.Transition.StateAfter = after;
    }

    void Apply()
    {
        if (_count && !_applied)
            _list->ResourceBarrier(_count, _barriers.data());
        _applied = true;
    }

    ~ResourceTransitions()
    {
        if (!_applied || !_count)
            return;
        for (unsigned i = 0; i < _count; ++i)
            std::swap(_barriers[i].Transition.StateBefore, _barriers[i].Transition.StateAfter);
        _list->ResourceBarrier(_count, _barriers.data());
    }
};
} // namespace HelixSR::Dx12
