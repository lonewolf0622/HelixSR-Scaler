// CPU-only: record transitions around an upscaler call without a D3D device.
#include "../../OptiScaler/helixsr/HelixSRResourceStates.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

using namespace HelixSR::Dx12;
static unsigned checks = 0;
#define CHECK(expr)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL %d: %s\n", __LINE__, #expr);                                                             \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

struct Params
{
    std::unordered_map<std::string, void*> pointers;
    std::unordered_map<std::string, unsigned> numbers;
    void Set(const char* key, void* value) { pointers[key] = value; }
    void Set(const char* key, unsigned value) { numbers[key] = value; }
    unsigned Get(const char* key, ID3D12Resource** value)
    {
        auto it = pointers.find(key);
        if (it == pointers.end())
            return 0;
        *value = static_cast<ID3D12Resource*>(it->second);
        return 1;
    }
    unsigned Get(const char* key, unsigned* value)
    {
        auto it = numbers.find(key);
        if (it == numbers.end())
            return 0;
        *value = it->second;
        return 1;
    }
};
struct Resource
{
    void* resource;
    unsigned state;
};
struct Dispatch
{
    Resource color, depth, motionVectors, exposure, reactive, output;
};
struct List
{
    std::vector<std::vector<D3D12_RESOURCE_BARRIER>> batches;
    void ResourceBarrier(unsigned n, const D3D12_RESOURCE_BARRIER* b) { batches.emplace_back(b, b + n); }
};
static ID3D12Resource* Texture(unsigned n) { return reinterpret_cast<ID3D12Resource*>((uintptr_t) n); }

static bool Evaluate(List& list, Params& params, ID3D12Resource* output, bool success)
{
    ResourceTransitions transitions(&list);
    for (unsigned i = 0; i < kResourceKeys.size(); ++i)
    {
        ID3D12Resource* resource = nullptr;
        params.Get(kResourceKeys[i].resource, &resource);
        if (i == 5)
            resource = output;
        transitions.Add(resource, InputState(&params, i, resource, std::nullopt),
                        i == 5 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transitions.Apply();
    CHECK(list.batches.size() == 1);
    return success; // exercise restoration on both success and error returns
}

int main()
{
    CHECK(!Fsr2State(0));
    CHECK(!Fsr2State(16));
    CHECK(Fsr2State(1) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(Fsr2State(2) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(Fsr2State(4) == D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(Fsr2State(8) == D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(Fsr2State(6) == (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    CHECK(!Fsr3State(0));
    CHECK(!Fsr3State(1024));
    CHECK(Fsr3State(512) == D3D12_RESOURCE_STATE_DEPTH_WRITE);
    CHECK(Fsr3State(1) == D3D12_RESOURCE_STATE_COMMON);
    CHECK(Fsr3State(128) == D3D12_RESOURCE_STATE_PRESENT);
    CHECK(Fsr3State(2) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(Fsr3State(4) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CHECK(Fsr3State(8) == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CHECK(Fsr3State(12) ==
          (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    CHECK(Fsr3State(16) == D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(Fsr3State(32) == D3D12_RESOURCE_STATE_COPY_DEST);
    CHECK(Fsr3State(20) == (D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    CHECK(Fsr3State(64) == D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    CHECK(Fsr3State(256) == D3D12_RESOURCE_STATE_RENDER_TARGET);

    Dispatch fsr2 { { Texture(1), 1 }, { Texture(2), 2 }, { Texture(3), 2 },
                    { nullptr, 0 },    { nullptr, 0 },    { Texture(6), 2 } };
    for (bool success : { false, true })
    {
        Params params;
        CaptureStates(&params, fsr2, Fsr2State);
        List list;
        CHECK(Evaluate(list, params, Texture(6), success) == success);
        CHECK(list.batches.size() == 2);
        CHECK(list.batches[0].size() == 2);
        const auto& color = list.batches[0][0];
        CHECK(color.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
        CHECK(color.Transition.pResource == Texture(1));
        CHECK(color.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        CHECK(color.Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CHECK(color.Transition.StateAfter == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CHECK(list.batches[0][1].Transition.pResource == Texture(6));
        CHECK(list.batches[0][1].Transition.StateBefore == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CHECK(list.batches[0][1].Transition.StateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (unsigned i = 0; i < 2; ++i)
        {
            CHECK(list.batches[1][i].Transition.pResource == list.batches[0][i].Transition.pResource);
            CHECK(list.batches[1][i].Transition.StateBefore == list.batches[0][i].Transition.StateAfter);
            CHECK(list.batches[1][i].Transition.StateAfter == list.batches[0][i].Transition.StateBefore);
        }
    }

    Params params;
    CaptureStates(&params, fsr2, Fsr2State);
    // RCAS/output scaling scratch must not inherit the game's output state.
    CHECK(!InputState(&params, 5, Texture(7), D3D12_RESOURCE_STATE_COPY_DEST));
    CHECK(!InputState(&params, 5, Texture(7), std::nullopt, D3D12_RESOURCE_STATE_COPY_DEST));
    CHECK(InputState(&params, 0, Texture(1), std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET) ==
          D3D12_RESOURCE_STATE_RENDER_TARGET);
    List redirected;
    CHECK(Evaluate(redirected, params, Texture(7), true));
    CHECK(redirected.batches[0].size() == 1);
    CHECK(redirected.batches[0][0].Transition.pResource == Texture(1));

    // Next frame can use the same texture in a different state.
    fsr2.color.state = 4;
    CaptureStates(&params, fsr2, Fsr2State);
    CHECK(InputState(&params, 0, Texture(1), std::nullopt) == D3D12_RESOURCE_STATE_COPY_SOURCE);
    fsr2.color.state = 0;
    CaptureStates(&params, fsr2, Fsr2State);
    CHECK(InputState(&params, 0, Texture(1), D3D12_RESOURCE_STATE_RENDER_TARGET) == D3D12_RESOURCE_STATE_RENDER_TARGET);

    Dispatch fsr3 { { Texture(1), 1 }, { Texture(2), 4 }, { Texture(3), 12 },
                    { Texture(4), 2 }, { Texture(5), 8 }, { Texture(6), 2 } };
    CaptureStates(&params, fsr3, Fsr3State);
    CHECK(InputState(&params, 0, Texture(1), std::nullopt) == D3D12_RESOURCE_STATE_COMMON);
    CHECK(InputState(&params, 4, Texture(5), std::nullopt) == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Native NGX has no FSR metadata: honor hotfixes and skip unspecified states.
    Params native;
    CHECK(InputState(&native, 0, Texture(1), D3D12_RESOURCE_STATE_RENDER_TARGET) == D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(!InputState(&native, 0, Texture(1), std::nullopt));
    List aliases;
    {
        ResourceTransitions transitions(&aliases);
        transitions.Add(nullptr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transitions.Add(Texture(1), std::nullopt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transitions.Add(Texture(2), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transitions.Add(Texture(3), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitions.Add(Texture(3), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transitions.Apply();
        transitions.Apply();
        CHECK(aliases.batches.size() == 1 && aliases.batches[0].size() == 1);
    }
    CHECK(aliases.batches.size() == 2);
    std::printf("PASS: %u resource-state checks (CPU only)\n", checks);
}
