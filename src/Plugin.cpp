#define NOMINMAX
#include "Plugin.h"
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3d12")

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "HookWrapper.hpp"
#include "IUnityInterface.h"
#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityLog.h"
#include "MinHook.h"


//-------------------------------------------------------
// Global Unity interfaces
static IUnityInterfaces *g_unityInterfaces = nullptr;
static IUnityGraphics *g_unityGraphics = nullptr;
IUnityLog *g_Log = nullptr;
static IUnityGraphicsD3D12v8 *g_unityGraphics_D3D12 = nullptr;


typedef decltype(&D3D12SerializeRootSignature) t_D3D12SerializeRootSignature;

typedef HRESULT (*t_CreateDescriptorHeap)(
    ID3D12Device *pThis,
    const D3D12_DESCRIPTOR_HEAP_DESC *pDescriptorHeapDesc,
    REFIID riid,
    void **ppvHeap);

typedef HRESULT (*t_CreateCommandList)(
    ID3D12Device *pThis,
    UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *pCommandAllocator,
    ID3D12PipelineState *pInitialState,
    REFIID riid,
    void **ppCommandList);

typedef HRESULT (*t_CreateCommandList1)(
    ID3D12Device4 *pThis,
    UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid,
    void **ppCommandList);

typedef void (*t_SetDescriptorHeaps)(
    ID3D12GraphicsCommandList *pThis,
    UINT NumDescriptorHeaps,
    ID3D12DescriptorHeap *const *ppDescriptorHeaps);

typedef HRESULT (*t_Reset)(
    ID3D12GraphicsCommandList *pThis,
    ID3D12CommandAllocator *pAllocator,
    ID3D12PipelineState *pInitialState);

static constexpr UINT kMainDescriptorHeapMinCount = 262144u;
static constexpr UINT kBindlessDescriptorReserveCount = 4096u * 4u;
static constexpr UINT kAbsoluteMaxDescriptorCount = 1000000u;
static constexpr size_t kCreateCommandListVTableIndex = 12u;
static constexpr size_t kCreateCommandList1VTableIndex = 54u;
static constexpr size_t kCreateDescriptorHeapVTableIndex = 14u;
static constexpr size_t kResetVTableIndex = 10u;
static constexpr size_t kSetDescriptorHeapsVTableIndex = 28u;
static constexpr size_t kMaxCreateCommandListHookSlots = 16u;
static constexpr size_t kMaxCreateCommandList1HookSlots = 16u;
static constexpr size_t kMaxCreateDescriptorHeapHookSlots = 16u;
static constexpr size_t kMaxCreateDescriptorHeapVTablePatchSlots = 32u;
static constexpr size_t kMaxResetHookSlots = 64u;
static constexpr size_t kMaxSetDescriptorHeapsHookSlots = 64u;

struct CreateCommandListHookState {
    HookWrapper<t_CreateCommandList> *hook = nullptr;
    const char *interfaceName = nullptr;
    void *target = nullptr;
};

struct CreateCommandList1HookState {
    HookWrapper<t_CreateCommandList1> *hook = nullptr;
    const char *interfaceName = nullptr;
    void *target = nullptr;
};

struct CreateDescriptorHeapHookState {
    HookWrapper<t_CreateDescriptorHeap> *hook = nullptr;
    t_CreateDescriptorHeap original = nullptr;
    const char *interfaceName = nullptr;
    void *target = nullptr;
};

struct CreateDescriptorHeapVTablePatchState {
    const char *interfaceName = nullptr;
    void **entry = nullptr;
    void *originalTarget = nullptr;
    size_t hookIndex = SIZE_MAX;
};

struct SetDescriptorHeapsHookState {
    HookWrapper<t_SetDescriptorHeaps> *hook = nullptr;
    std::string label;
    void *target = nullptr;
};

struct ResetHookState {
    HookWrapper<t_Reset> *hook = nullptr;
    std::string label;
    void *target = nullptr;
};

struct DescriptorHeapMetadata {
    UINT totalDescriptorCount = 0;
    UINT bindlessDescriptorStart = 0;
    UINT bindlessDescriptorCount = 0;
    bool pluginOwnedRange = false;
};

struct DescriptorHeapMetadataLookupResult {
    DescriptorHeapMetadata metadata = {};
    const char *source = "default";
};

struct DescriptorHeapSelectionState {
    ID3D12DescriptorHeap *heap = nullptr;
    UINT incrementSize = 0;
    SIZE_T cpuDescriptorHandleForHeapStart = 0;
    UINT totalDescriptorCount = 0;
    UINT bindlessDescriptorStart = 0;
    UINT bindlessDescriptorCount = 0;
    bool pluginOwnedRange = false;
    std::string captureSource = "none";
};

static HookWrapper<t_D3D12SerializeRootSignature> *s_pSerializeRootSignatureHook = nullptr;
static std::array<CreateCommandListHookState, kMaxCreateCommandListHookSlots> s_createCommandListHooks = {};
static size_t s_createCommandListHookCount = 0;
static std::array<CreateCommandList1HookState, kMaxCreateCommandList1HookSlots> s_createCommandList1Hooks = {};
static size_t s_createCommandList1HookCount = 0;
static std::array<CreateDescriptorHeapHookState, kMaxCreateDescriptorHeapHookSlots> s_createDescriptorHeapHooks = {};
static size_t s_createDescriptorHeapHookCount = 0;
static std::array<CreateDescriptorHeapVTablePatchState, kMaxCreateDescriptorHeapVTablePatchSlots> s_createDescriptorHeapVTablePatches = {};
static size_t s_createDescriptorHeapVTablePatchCount = 0;
static std::array<ResetHookState, kMaxResetHookSlots> s_resetHooks = {};
static size_t s_resetHookCount = 0;
static std::array<SetDescriptorHeapsHookState, kMaxSetDescriptorHeapsHookSlots> s_setDescriptorHeapsHooks = {};
static size_t s_setDescriptorHeapsHookCount = 0;
static std::string s_createCommandListHookReport = "not attempted";
static std::string s_createCommandList1HookReport = "not attempted";
static std::string s_createDescriptorHeapHookReport = "not attempted";
static std::string s_resetHookReport = "not attempted";
static std::string s_setDescriptorHeapsHookReport = "not attempted";
static std::unordered_map<ID3D12DescriptorHeap *, DescriptorHeapMetadata> s_descriptorHeapMetadata = {};
static std::unordered_map<SIZE_T, DescriptorHeapMetadata> s_descriptorHeapMetadataByCpuStart = {};
static DescriptorHeapSelectionState s_descriptorHeapSelection = {};
static DescriptorHeapSelectionState s_pluginOwnedDescriptorHeapSelection = {};
static std::string s_lastDescriptorHeapUnavailableMessage;
static uint64_t s_setDescriptorHeapsObservedCount = 0;
static thread_local uint32_t s_createDescriptorHeapDetourDepth = 0;

struct ScopedCreateDescriptorHeapDetourDepth {
    ScopedCreateDescriptorHeapDetourDepth()
        : m_isReentrant(s_createDescriptorHeapDetourDepth > 0) {
        ++s_createDescriptorHeapDetourDepth;
    }

    ~ScopedCreateDescriptorHeapDetourDepth() {
        --s_createDescriptorHeapDetourDepth;
    }

    [[nodiscard]] bool IsReentrant() const {
        return m_isReentrant;
    }

private:
    bool m_isReentrant = false;
};

static void LogInfo(const std::string &message) {
    if (g_Log != nullptr) {
        UNITY_LOG(g_Log, message.c_str());
    }
}

static void LogError(const std::string &message) {
    if (g_Log != nullptr) {
        UNITY_LOG_ERROR(g_Log, message.c_str());
    }
}

static const char *RendererToString(const UnityGfxRenderer renderer) {
    switch (renderer) {
        case kUnityGfxRendererD3D11:
            return "D3D11";
        case kUnityGfxRendererD3D12:
            return "D3D12";
        case kUnityGfxRendererOpenGLCore:
            return "OpenGLCore";
        case kUnityGfxRendererVulkan:
            return "Vulkan";
        case kUnityGfxRendererMetal:
            return "Metal";
        case kUnityGfxRendererNull:
            return "Null";
        default:
            return "Unknown";
    }
}

static const char *DescriptorHeapTypeToString(const D3D12_DESCRIPTOR_HEAP_TYPE type) {
    switch (type) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
            return "CBV_SRV_UAV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
            return "SAMPLER";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
            return "RTV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
            return "DSV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES:
            return "NUM_TYPES";
        default:
            return "UNKNOWN";
    }
}

static std::string DescriptorHeapFlagsToString(const D3D12_DESCRIPTOR_HEAP_FLAGS flags) {
    if (flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE) {
        return "NONE";
    }

    std::string result;
    if ((flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        result += "SHADER_VISIBLE";
    }

    return result;
}

static std::string PointerToString(const void *ptr) {
    return std::format("{:p}", ptr);
}

static std::string ModuleHandleToString(HMODULE module) {
    if (module == nullptr) {
        return "nullptr";
    }

    std::array<char, MAX_PATH> path = {};
    const DWORD length = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) {
        return PointerToString(module);
    }

    return std::format("{} ({})", PointerToString(module), std::string(path.data(), length));
}

static std::string ModuleForAddressToString(const void *address) {
    if (address == nullptr) {
        return "nullptr";
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(address),
        &module
    )) {
        return "unknown";
    }

    return ModuleHandleToString(module);
}

static std::string AddressWithModuleToString(const void *address) {
    return std::format("{} [{}]", PointerToString(address), ModuleForAddressToString(address));
}

static bool PatchPointerWithWritableProtection(void **entry, void *value) {
    if (entry == nullptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION memoryInfo = {};
    if (VirtualQuery(entry, &memoryInfo, sizeof(memoryInfo)) == 0) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(memoryInfo.BaseAddress, memoryInfo.RegionSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    *entry = value;
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void *));

    DWORD restoredProtect = 0;
    const BOOL restored = VirtualProtect(memoryInfo.BaseAddress, memoryInfo.RegionSize, oldProtect, &restoredProtect);
    return restored != FALSE;
}

static void AppendDiagnosticInfo(std::string &report, const std::string &message) {
    if (!report.empty()) {
        report += "; ";
    }

    report += message;
}

static std::string BuildCreateDescriptorHeapHookInfo() {
    if (s_createDescriptorHeapHookCount == 0) {
        return "none";
    }

    std::string result;
    for (size_t i = 0; i < s_createDescriptorHeapHookCount; ++i) {
        const CreateDescriptorHeapHookState &hookState = s_createDescriptorHeapHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        AppendDiagnosticInfo(
            result,
            std::format(
                "{}={}",
                hookState.interfaceName != nullptr ? hookState.interfaceName : "unknown",
                AddressWithModuleToString(hookState.target)
            )
        );
    }

    return result.empty() ? "none" : result;
}

static std::string BuildCreateCommandListHookInfo() {
    if (s_createCommandListHookCount == 0) {
        return "none";
    }

    std::string result;
    for (size_t i = 0; i < s_createCommandListHookCount; ++i) {
        const CreateCommandListHookState &hookState = s_createCommandListHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        AppendDiagnosticInfo(
            result,
            std::format(
                "{}={}",
                hookState.interfaceName != nullptr ? hookState.interfaceName : "unknown",
                AddressWithModuleToString(hookState.target)
            )
        );
    }

    return result.empty() ? "none" : result;
}

static std::string BuildCreateCommandList1HookInfo() {
    if (s_createCommandList1HookCount == 0) {
        return "none";
    }

    std::string result;
    for (size_t i = 0; i < s_createCommandList1HookCount; ++i) {
        const CreateCommandList1HookState &hookState = s_createCommandList1Hooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        AppendDiagnosticInfo(
            result,
            std::format(
                "{}={}",
                hookState.interfaceName != nullptr ? hookState.interfaceName : "unknown",
                AddressWithModuleToString(hookState.target)
            )
        );
    }

    return result.empty() ? "none" : result;
}

static std::string BuildResetHookInfo() {
    if (s_resetHookCount == 0) {
        return "none";
    }

    std::string result;
    for (size_t i = 0; i < s_resetHookCount; ++i) {
        const ResetHookState &hookState = s_resetHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        AppendDiagnosticInfo(
            result,
            std::format("{}={}", hookState.label, AddressWithModuleToString(hookState.target))
        );
    }

    return result.empty() ? "none" : result;
}

static std::string BuildSetDescriptorHeapsHookInfo() {
    if (s_setDescriptorHeapsHookCount == 0) {
        return "none";
    }

    std::string result;
    for (size_t i = 0; i < s_setDescriptorHeapsHookCount; ++i) {
        const SetDescriptorHeapsHookState &hookState = s_setDescriptorHeapsHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        AppendDiagnosticInfo(
            result,
            std::format("{}={}", hookState.label, AddressWithModuleToString(hookState.target))
        );
    }

    return result.empty() ? "none" : result;
}

static size_t FindCreateDescriptorHeapHookIndexByTarget(const void *target) {
    for (size_t i = 0; i < s_createDescriptorHeapHookCount; ++i) {
        const CreateDescriptorHeapHookState &hookState = s_createDescriptorHeapHooks[i];
        if (hookState.target == target) {
            return i;
        }
    }

    return SIZE_MAX;
}

static size_t FindCreateDescriptorHeapVTablePatchIndexByEntry(void **entry) {
    for (size_t i = 0; i < s_createDescriptorHeapVTablePatchCount; ++i) {
        const CreateDescriptorHeapVTablePatchState &patchState = s_createDescriptorHeapVTablePatches[i];
        if (patchState.entry == entry) {
            return i;
        }
    }

    return SIZE_MAX;
}

static std::string HeapDescToString(const D3D12_DESCRIPTOR_HEAP_DESC &desc) {
    return std::format(
        "type={}, flags={}, shaderVisible={}, numDescriptors={}, nodeMask={}",
        DescriptorHeapTypeToString(desc.Type),
        DescriptorHeapFlagsToString(desc.Flags),
        (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0,
        desc.NumDescriptors,
        desc.NodeMask
    );
}

static std::string DescriptorHeapMetadataToString(const DescriptorHeapMetadata &metadata) {
    return std::format(
        "pluginOwnedRange={}, bindlessStart={}, bindlessCount={}, totalCount={}",
        metadata.pluginOwnedRange,
        metadata.bindlessDescriptorStart,
        metadata.bindlessDescriptorCount,
        metadata.totalDescriptorCount
    );
}

static SIZE_T GetDescriptorHeapCpuStart(ID3D12DescriptorHeap *pHeap) {
    if (pHeap == nullptr) {
        return 0;
    }

    return pHeap->GetCPUDescriptorHandleForHeapStart().ptr;
}

static void NormalizeDescriptorHeapMetadata(
    const D3D12_DESCRIPTOR_HEAP_DESC &desc,
    DescriptorHeapMetadata &metadata
) {
    metadata.totalDescriptorCount = desc.NumDescriptors;
    if (metadata.bindlessDescriptorStart > metadata.totalDescriptorCount) {
        metadata.bindlessDescriptorStart = metadata.totalDescriptorCount;
    }

    const uint64_t bindlessRangeEnd = static_cast<uint64_t>(metadata.bindlessDescriptorStart)
        + static_cast<uint64_t>(metadata.bindlessDescriptorCount);
    if (bindlessRangeEnd > metadata.totalDescriptorCount) {
        metadata.bindlessDescriptorCount = metadata.totalDescriptorCount - metadata.bindlessDescriptorStart;
    }

    metadata.pluginOwnedRange = metadata.bindlessDescriptorCount > 0;
}

static void StoreDescriptorHeapMetadata(
    ID3D12DescriptorHeap *pHeap,
    const DescriptorHeapMetadata &metadata
) {
    if (pHeap == nullptr) {
        return;
    }

    s_descriptorHeapMetadata[pHeap] = metadata;

    const SIZE_T cpuStart = GetDescriptorHeapCpuStart(pHeap);
    if (cpuStart != 0) {
        s_descriptorHeapMetadataByCpuStart[cpuStart] = metadata;
    }
}

static void ResetDescriptorHeapTrackingState() {
    s_descriptorHeapMetadata.clear();
    s_descriptorHeapMetadataByCpuStart.clear();
    s_descriptorHeapSelection = {};
    s_descriptorHeapSelection.captureSource = "none";
    s_pluginOwnedDescriptorHeapSelection = {};
    s_pluginOwnedDescriptorHeapSelection.captureSource = "none";
    s_lastDescriptorHeapUnavailableMessage.clear();
    s_setDescriptorHeapsObservedCount = 0;
}

static bool IsShaderVisibleCBVSRVUAVHeap(ID3D12DescriptorHeap *pHeap) {
    if (pHeap == nullptr) {
        return false;
    }

    const D3D12_DESCRIPTOR_HEAP_DESC desc = pHeap->GetDesc();
    return desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        && (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
}

static DescriptorHeapMetadataLookupResult LookupDescriptorHeapMetadata(ID3D12DescriptorHeap *pHeap) {
    const D3D12_DESCRIPTOR_HEAP_DESC desc = pHeap->GetDesc();

    DescriptorHeapMetadataLookupResult result = {};
    result.metadata.totalDescriptorCount = desc.NumDescriptors;
    result.metadata.bindlessDescriptorStart = desc.NumDescriptors;
    result.metadata.bindlessDescriptorCount = 0;
    result.metadata.pluginOwnedRange = false;

    const auto iterator = s_descriptorHeapMetadata.find(pHeap);
    if (iterator != s_descriptorHeapMetadata.end()) {
        result.metadata = iterator->second;
        result.source = "pointer";
    } else {
        const SIZE_T cpuStart = GetDescriptorHeapCpuStart(pHeap);
        if (cpuStart != 0) {
            const auto cpuStartIterator = s_descriptorHeapMetadataByCpuStart.find(cpuStart);
            if (cpuStartIterator != s_descriptorHeapMetadataByCpuStart.end()) {
                result.metadata = cpuStartIterator->second;
                result.source = "cpuStart";
                LogInfo(std::format(
                    "Recovered descriptor heap metadata by CPU start: heap={}, cpuStart=0x{:X}, {}",
                    PointerToString(pHeap),
                    static_cast<unsigned long long>(cpuStart),
                    DescriptorHeapMetadataToString(result.metadata)
                ));
            }
        }

        if (std::string_view(result.source) == "default"
            && desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
            && (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0
            && desc.NumDescriptors >= kMainDescriptorHeapMinCount) {
            LogError(std::format(
                "Descriptor heap metadata lookup missed for a large shader-visible CBV/SRV/UAV heap: heap={}, cpuStart=0x{:X}, {}. CreateDescriptorHeap may not have been detoured for this heap.",
                PointerToString(pHeap),
                static_cast<unsigned long long>(cpuStart),
                HeapDescToString(desc)
            ));
        }
    }

    NormalizeDescriptorHeapMetadata(desc, result.metadata);
    StoreDescriptorHeapMetadata(pHeap, result.metadata);
    return result;
}

static bool HasPluginOwnedBindlessRange(const DescriptorHeapSelectionState &selection) {
    return selection.heap != nullptr
        && selection.pluginOwnedRange
        && selection.bindlessDescriptorCount > 0;
}

static const DescriptorHeapSelectionState *GetDescriptorHeapSelectionForQueries() {
    if (s_descriptorHeapSelection.heap != nullptr) {
        return &s_descriptorHeapSelection;
    }

    if (s_pluginOwnedDescriptorHeapSelection.heap != nullptr) {
        return &s_pluginOwnedDescriptorHeapSelection;
    }

    return nullptr;
}

static const DescriptorHeapSelectionState *GetBindlessDescriptorHeapSelection() {
    if (HasPluginOwnedBindlessRange(s_descriptorHeapSelection)) {
        return &s_descriptorHeapSelection;
    }

    if (HasPluginOwnedBindlessRange(s_pluginOwnedDescriptorHeapSelection)) {
        return &s_pluginOwnedDescriptorHeapSelection;
    }

    return nullptr;
}

static const DescriptorHeapSelectionState *GetDescriptorHeapSelectionForPrebind() {
    const DescriptorHeapSelectionState *bindlessSelection = GetBindlessDescriptorHeapSelection();
    if (bindlessSelection != nullptr && bindlessSelection->heap != nullptr) {
        return bindlessSelection;
    }

    return GetDescriptorHeapSelectionForQueries();
}

static t_SetDescriptorHeaps ResolveOriginalSetDescriptorHeaps(
    ID3D12GraphicsCommandList *pCommandList
) {
    if (pCommandList == nullptr) {
        return nullptr;
    }

    void **pCommandListVTable = *reinterpret_cast<void ***>(pCommandList);
    void *fnSetDescriptorHeaps = pCommandListVTable[kSetDescriptorHeapsVTableIndex];
    for (size_t i = 0; i < s_setDescriptorHeapsHookCount; ++i) {
        const SetDescriptorHeapsHookState &hookState = s_setDescriptorHeapsHooks[i];
        if (hookState.target == fnSetDescriptorHeaps && hookState.hook != nullptr) {
            return hookState.hook->GetOriginalPtr();
        }
    }

    return reinterpret_cast<t_SetDescriptorHeaps>(fnSetDescriptorHeaps);
}

static void PrebindDescriptorHeapIfAvailable(
    ID3D12GraphicsCommandList *pCommandList,
    const D3D12_COMMAND_LIST_TYPE commandListType
) {
    if (pCommandList == nullptr || commandListType != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    const DescriptorHeapSelectionState *selection = GetDescriptorHeapSelectionForPrebind();
    if (selection == nullptr || !IsShaderVisibleCBVSRVUAVHeap(selection->heap)) {
        return;
    }

    const t_SetDescriptorHeaps originalSetDescriptorHeaps = ResolveOriginalSetDescriptorHeaps(pCommandList);
    if (originalSetDescriptorHeaps == nullptr) {
        return;
    }

    ID3D12DescriptorHeap *heaps[] = {selection->heap};
    originalSetDescriptorHeaps(pCommandList, static_cast<UINT>(std::size(heaps)), heaps);
}

static void PrebindDescriptorHeapForCreatedCommandListIfAvailable(
    void *pCommandListObject,
    const D3D12_COMMAND_LIST_TYPE commandListType
) {
    if (pCommandListObject == nullptr || commandListType != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> pGraphicsCommandList;
    IUnknown *pUnknown = reinterpret_cast<IUnknown *>(pCommandListObject);
    if (FAILED(pUnknown->QueryInterface(IID_PPV_ARGS(pGraphicsCommandList.GetAddressOf())))) {
        return;
    }

    PrebindDescriptorHeapIfAvailable(pGraphicsCommandList.Get(), commandListType);
}

static void CachePluginOwnedDescriptorHeap(
    ID3D12Device *pDevice,
    ID3D12DescriptorHeap *pHeap,
    const DescriptorHeapMetadata &metadata,
    const std::string &captureSource
) {
    if (pHeap == nullptr || !metadata.pluginOwnedRange) {
        return;
    }

    const UINT incrementSize =
        pDevice != nullptr ? pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;
    const SIZE_T cpuDescriptorHandleForHeapStart = GetDescriptorHeapCpuStart(pHeap);
    const bool selectionChanged =
        s_pluginOwnedDescriptorHeapSelection.heap != pHeap
        || s_pluginOwnedDescriptorHeapSelection.incrementSize != incrementSize
        || s_pluginOwnedDescriptorHeapSelection.cpuDescriptorHandleForHeapStart != cpuDescriptorHandleForHeapStart
        || s_pluginOwnedDescriptorHeapSelection.totalDescriptorCount != metadata.totalDescriptorCount
        || s_pluginOwnedDescriptorHeapSelection.bindlessDescriptorStart != metadata.bindlessDescriptorStart
        || s_pluginOwnedDescriptorHeapSelection.bindlessDescriptorCount != metadata.bindlessDescriptorCount
        || s_pluginOwnedDescriptorHeapSelection.pluginOwnedRange != metadata.pluginOwnedRange
        || s_pluginOwnedDescriptorHeapSelection.captureSource != captureSource;

    s_pluginOwnedDescriptorHeapSelection.heap = pHeap;
    s_pluginOwnedDescriptorHeapSelection.incrementSize = incrementSize;
    s_pluginOwnedDescriptorHeapSelection.cpuDescriptorHandleForHeapStart = cpuDescriptorHandleForHeapStart;
    s_pluginOwnedDescriptorHeapSelection.totalDescriptorCount = metadata.totalDescriptorCount;
    s_pluginOwnedDescriptorHeapSelection.bindlessDescriptorStart = metadata.bindlessDescriptorStart;
    s_pluginOwnedDescriptorHeapSelection.bindlessDescriptorCount = metadata.bindlessDescriptorCount;
    s_pluginOwnedDescriptorHeapSelection.pluginOwnedRange = metadata.pluginOwnedRange;
    s_pluginOwnedDescriptorHeapSelection.captureSource = captureSource;

    if (selectionChanged) {
        const D3D12_DESCRIPTOR_HEAP_DESC capturedDesc = pHeap->GetDesc();
        LogInfo(std::format(
            "Cached plugin-owned bindless heap from {}: heap={}, {}, {}, incrementSize={}, cpuStart=0x{:X}",
            captureSource,
            PointerToString(pHeap),
            HeapDescToString(capturedDesc),
            DescriptorHeapMetadataToString(metadata),
            incrementSize,
            static_cast<unsigned long long>(cpuDescriptorHandleForHeapStart)
        ));
    }
}

static bool ShouldReplaceSelectedDescriptorHeap(
    ID3D12DescriptorHeap *pHeap,
    const DescriptorHeapMetadata &metadata,
    const std::string &captureSource
) {
    if (pHeap == nullptr) {
        return false;
    }

    if (s_descriptorHeapSelection.heap == nullptr || s_descriptorHeapSelection.heap == pHeap) {
        return true;
    }

    if (metadata.pluginOwnedRange != s_descriptorHeapSelection.pluginOwnedRange) {
        return metadata.pluginOwnedRange;
    }

    if (metadata.totalDescriptorCount != s_descriptorHeapSelection.totalDescriptorCount) {
        return metadata.totalDescriptorCount > s_descriptorHeapSelection.totalDescriptorCount;
    }

    if (metadata.bindlessDescriptorCount != s_descriptorHeapSelection.bindlessDescriptorCount) {
        return metadata.bindlessDescriptorCount > s_descriptorHeapSelection.bindlessDescriptorCount;
    }

    const bool incomingIsRuntimeBinding = captureSource == "SetDescriptorHeaps";
    const bool currentIsRuntimeBinding = s_descriptorHeapSelection.captureSource == "SetDescriptorHeaps";
    if (incomingIsRuntimeBinding != currentIsRuntimeBinding) {
        return incomingIsRuntimeBinding;
    }

    return false;
}

static void CacheDescriptorHeap(
    ID3D12Device *pDevice,
    ID3D12DescriptorHeap *pHeap,
    const char *source
) {
    if (!IsShaderVisibleCBVSRVUAVHeap(pHeap)) {
        return;
    }

    const DescriptorHeapMetadataLookupResult metadataLookup = LookupDescriptorHeapMetadata(pHeap);
    const DescriptorHeapMetadata &metadata = metadataLookup.metadata;
    const std::string captureSource = source != nullptr ? source : "unknown";
    CachePluginOwnedDescriptorHeap(pDevice, pHeap, metadata, captureSource);
    if (!ShouldReplaceSelectedDescriptorHeap(pHeap, metadata, captureSource)) {
        return;
    }

    const UINT incrementSize =
        pDevice != nullptr ? pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;
    const SIZE_T cpuDescriptorHandleForHeapStart = GetDescriptorHeapCpuStart(pHeap);
    const bool selectionChanged =
        s_descriptorHeapSelection.heap != pHeap
        || s_descriptorHeapSelection.incrementSize != incrementSize
        || s_descriptorHeapSelection.cpuDescriptorHandleForHeapStart != cpuDescriptorHandleForHeapStart
        || s_descriptorHeapSelection.totalDescriptorCount != metadata.totalDescriptorCount
        || s_descriptorHeapSelection.bindlessDescriptorStart != metadata.bindlessDescriptorStart
        || s_descriptorHeapSelection.bindlessDescriptorCount != metadata.bindlessDescriptorCount
        || s_descriptorHeapSelection.pluginOwnedRange != metadata.pluginOwnedRange
        || s_descriptorHeapSelection.captureSource != captureSource;

    s_descriptorHeapSelection.heap = pHeap;
    s_descriptorHeapSelection.incrementSize = incrementSize;
    s_descriptorHeapSelection.cpuDescriptorHandleForHeapStart = cpuDescriptorHandleForHeapStart;
    s_descriptorHeapSelection.totalDescriptorCount = metadata.totalDescriptorCount;
    s_descriptorHeapSelection.bindlessDescriptorStart = metadata.bindlessDescriptorStart;
    s_descriptorHeapSelection.bindlessDescriptorCount = metadata.bindlessDescriptorCount;
    s_descriptorHeapSelection.pluginOwnedRange = metadata.pluginOwnedRange;
    s_descriptorHeapSelection.captureSource = captureSource;
    s_lastDescriptorHeapUnavailableMessage.clear();

    if (selectionChanged) {
        const D3D12_DESCRIPTOR_HEAP_DESC capturedDesc = pHeap->GetDesc();
        LogInfo(std::format(
            "Selected shader-visible CBV/SRV/UAV heap from {}: heap={}, {}, {}, incrementSize={}, cpuStart=0x{:X}, metadataSource={}",
            s_descriptorHeapSelection.captureSource,
            PointerToString(s_descriptorHeapSelection.heap),
            HeapDescToString(capturedDesc),
            DescriptorHeapMetadataToString(metadata),
            s_descriptorHeapSelection.incrementSize,
            static_cast<unsigned long long>(s_descriptorHeapSelection.cpuDescriptorHandleForHeapStart),
            metadataLookup.source
        ));
    }
}

static bool HasPluginOwnedBindlessRange() {
    return GetBindlessDescriptorHeapSelection() != nullptr;
}

static bool IsBindlessDescriptorIndexInRange(const uint32_t index) {
    const DescriptorHeapSelectionState *selection = GetBindlessDescriptorHeapSelection();
    if (selection == nullptr) {
        return false;
    }

    const uint64_t bindlessStart = selection->bindlessDescriptorStart;
    const uint64_t bindlessEnd = bindlessStart + selection->bindlessDescriptorCount;
    return index >= bindlessStart && index < bindlessEnd;
}

static void LogDescriptorHeapUnavailable(const char *context) {
    const ID3D12Device *pDevice = g_unityGraphics_D3D12 != nullptr ? g_unityGraphics_D3D12->GetDevice() : nullptr;
    const DescriptorHeapSelectionState *activeSelection = GetDescriptorHeapSelectionForQueries();
    const DescriptorHeapSelectionState *bindlessSelection = GetBindlessDescriptorHeapSelection();
    const std::string message = std::format(
        "{}: renderer={}, unityD3D12={}, device={}, activeHeap={}, activeBindlessHeap={}, cachedHeap={}, totalCount={}, bindlessStart={}, bindlessCount={}, pluginOwnedRange={}, incrementSize={}, cpuStart=0x{:X}, captureSource={}, pluginOwnedHeap={}, pluginOwnedTotalCount={}, pluginOwnedBindlessStart={}, pluginOwnedBindlessCount={}, pluginOwnedIncrementSize={}, pluginOwnedCpuStart=0x{:X}, pluginOwnedCaptureSource={}, createDescriptorHeapHooks={}, createDescriptorHeapReport={}, setDescriptorHeapsHooks={}, setDescriptorHeapsReport={}, setDescriptorHeapsObserved={}",
        context != nullptr ? context : "Descriptor heap unavailable",
        RendererToString(g_unityGraphics != nullptr ? g_unityGraphics->GetRenderer() : kUnityGfxRendererNull),
        PointerToString(g_unityGraphics_D3D12),
        PointerToString(pDevice),
        PointerToString(activeSelection != nullptr ? activeSelection->heap : nullptr),
        PointerToString(bindlessSelection != nullptr ? bindlessSelection->heap : nullptr),
        PointerToString(s_descriptorHeapSelection.heap),
        s_descriptorHeapSelection.totalDescriptorCount,
        s_descriptorHeapSelection.bindlessDescriptorStart,
        s_descriptorHeapSelection.bindlessDescriptorCount,
        s_descriptorHeapSelection.pluginOwnedRange,
        s_descriptorHeapSelection.incrementSize,
        static_cast<unsigned long long>(s_descriptorHeapSelection.cpuDescriptorHandleForHeapStart),
        s_descriptorHeapSelection.captureSource,
        PointerToString(s_pluginOwnedDescriptorHeapSelection.heap),
        s_pluginOwnedDescriptorHeapSelection.totalDescriptorCount,
        s_pluginOwnedDescriptorHeapSelection.bindlessDescriptorStart,
        s_pluginOwnedDescriptorHeapSelection.bindlessDescriptorCount,
        s_pluginOwnedDescriptorHeapSelection.incrementSize,
        static_cast<unsigned long long>(s_pluginOwnedDescriptorHeapSelection.cpuDescriptorHandleForHeapStart),
        s_pluginOwnedDescriptorHeapSelection.captureSource,
        BuildCreateDescriptorHeapHookInfo(),
        s_createDescriptorHeapHookReport,
        BuildSetDescriptorHeapsHookInfo(),
        s_setDescriptorHeapsHookReport,
        s_setDescriptorHeapsObservedCount
    );

    if (message != s_lastDescriptorHeapUnavailableMessage) {
        LogError(message);
        s_lastDescriptorHeapUnavailableMessage = message;
    }
}
static DescriptorHeapMetadata BuildDescriptorHeapMetadataForCreate(
    const D3D12_DESCRIPTOR_HEAP_DESC &requestedDesc,
    D3D12_DESCRIPTOR_HEAP_DESC &patchedDesc
) {
    patchedDesc = requestedDesc;

    DescriptorHeapMetadata metadata = {};
    metadata.totalDescriptorCount = requestedDesc.NumDescriptors;
    metadata.bindlessDescriptorStart = requestedDesc.NumDescriptors;
    metadata.bindlessDescriptorCount = 0;
    metadata.pluginOwnedRange = false;

    if (requestedDesc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        || (requestedDesc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0
        || requestedDesc.NumDescriptors < kMainDescriptorHeapMinCount) {
        return metadata;
    }

    const UINT maxAdditionalCount = requestedDesc.NumDescriptors < kAbsoluteMaxDescriptorCount
        ? kAbsoluteMaxDescriptorCount - requestedDesc.NumDescriptors
        : 0u;
    const UINT reserveCount = std::min(kBindlessDescriptorReserveCount, maxAdditionalCount);
    if (reserveCount == 0u) {
        return metadata;
    }

    patchedDesc.NumDescriptors = requestedDesc.NumDescriptors + reserveCount;
    metadata.totalDescriptorCount = patchedDesc.NumDescriptors;
    metadata.bindlessDescriptorStart = requestedDesc.NumDescriptors;
    metadata.bindlessDescriptorCount = reserveCount;
    metadata.pluginOwnedRange = true;
    return metadata;
}

static HRESULT WINAPI DetourD3D12SerializeRootSignature(
    _In_ const D3D12_ROOT_SIGNATURE_DESC *pRootSignature,
    _In_ D3D_ROOT_SIGNATURE_VERSION Version,
    _Out_ ID3DBlob **ppBlob,
    _Always_(_Outptr_opt_result_maybenull_) ID3DBlob **ppErrorBlob
) {
    D3D12_ROOT_SIGNATURE_DESC desc = *pRootSignature;
    if ((desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) == 0) {
        desc.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    }

    const HRESULT result = s_pSerializeRootSignatureHook->GetOriginalPtr()(&desc, Version, ppBlob, ppErrorBlob);
    if (FAILED(result)) {
        UNITY_LOG_ERROR(g_Log, "Serializing root signature failure");
    }

    return result;
}

static HRESULT DetourCreateCommandListImpl(
    size_t hookIndex,
    ID3D12Device *pThis,
    UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *pCommandAllocator,
    ID3D12PipelineState *pInitialState,
    REFIID riid,
    void **ppCommandList
) {
    if (hookIndex >= s_createCommandListHookCount) {
        LogError(std::format(
            "DetourCreateCommandList invoked with invalid hook slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const CreateCommandListHookState &hookState = s_createCommandListHooks[hookIndex];
    if (hookState.hook == nullptr) {
        LogError(std::format(
            "DetourCreateCommandList invoked with missing hook state at slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const HRESULT hr = hookState.hook->GetOriginalPtr()(
        pThis,
        nodeMask,
        type,
        pCommandAllocator,
        pInitialState,
        riid,
        ppCommandList
    );
    if (SUCCEEDED(hr) && ppCommandList != nullptr && *ppCommandList != nullptr) {
        PrebindDescriptorHeapForCreatedCommandListIfAvailable(*ppCommandList, type);
    }

    return hr;
}

template<size_t HookIndex>
static HRESULT WINAPI DetourCreateCommandListSlot(
    ID3D12Device *pThis,
    UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    ID3D12CommandAllocator *pCommandAllocator,
    ID3D12PipelineState *pInitialState,
    REFIID riid,
    void **ppCommandList
) {
    return DetourCreateCommandListImpl(
        HookIndex,
        pThis,
        nodeMask,
        type,
        pCommandAllocator,
        pInitialState,
        riid,
        ppCommandList
    );
}

template<size_t... Indices>
static constexpr std::array<t_CreateCommandList, sizeof...(Indices)> MakeCreateCommandListDetours(
    std::index_sequence<Indices...>
) {
    return {&DetourCreateCommandListSlot<Indices>...};
}

static constexpr auto kCreateCommandListDetours = MakeCreateCommandListDetours(
    std::make_index_sequence<kMaxCreateCommandListHookSlots>()
);

static HRESULT DetourCreateCommandList1Impl(
    size_t hookIndex,
    ID3D12Device4 *pThis,
    UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid,
    void **ppCommandList
) {
    if (hookIndex >= s_createCommandList1HookCount) {
        LogError(std::format(
            "DetourCreateCommandList1 invoked with invalid hook slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const CreateCommandList1HookState &hookState = s_createCommandList1Hooks[hookIndex];
    if (hookState.hook == nullptr) {
        LogError(std::format(
            "DetourCreateCommandList1 invoked with missing hook state at slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const HRESULT hr = hookState.hook->GetOriginalPtr()(
        pThis,
        nodeMask,
        type,
        flags,
        riid,
        ppCommandList
    );
    if (SUCCEEDED(hr) && ppCommandList != nullptr && *ppCommandList != nullptr) {
        PrebindDescriptorHeapForCreatedCommandListIfAvailable(*ppCommandList, type);
    }

    return hr;
}

template<size_t HookIndex>
static HRESULT WINAPI DetourCreateCommandList1Slot(
    ID3D12Device4 *pThis,
    UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    D3D12_COMMAND_LIST_FLAGS flags,
    REFIID riid,
    void **ppCommandList
) {
    return DetourCreateCommandList1Impl(HookIndex, pThis, nodeMask, type, flags, riid, ppCommandList);
}

template<size_t... Indices>
static constexpr std::array<t_CreateCommandList1, sizeof...(Indices)> MakeCreateCommandList1Detours(
    std::index_sequence<Indices...>
) {
    return {&DetourCreateCommandList1Slot<Indices>...};
}

static constexpr auto kCreateCommandList1Detours = MakeCreateCommandList1Detours(
    std::make_index_sequence<kMaxCreateCommandList1HookSlots>()
);

static HRESULT DetourResetImpl(
    size_t hookIndex,
    ID3D12GraphicsCommandList *pThis,
    ID3D12CommandAllocator *pAllocator,
    ID3D12PipelineState *pInitialState
) {
    if (hookIndex >= s_resetHookCount) {
        LogError(std::format(
            "DetourReset invoked with invalid hook slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const ResetHookState &hookState = s_resetHooks[hookIndex];
    if (hookState.hook == nullptr) {
        LogError(std::format(
            "DetourReset invoked with missing hook state at slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const HRESULT hr = hookState.hook->GetOriginalPtr()(pThis, pAllocator, pInitialState);
    if (SUCCEEDED(hr) && pThis != nullptr) {
        PrebindDescriptorHeapIfAvailable(pThis, pThis->GetType());
    }

    return hr;
}

template<size_t HookIndex>
static HRESULT WINAPI DetourResetSlot(
    ID3D12GraphicsCommandList *pThis,
    ID3D12CommandAllocator *pAllocator,
    ID3D12PipelineState *pInitialState
) {
    return DetourResetImpl(HookIndex, pThis, pAllocator, pInitialState);
}

template<size_t... Indices>
static constexpr std::array<t_Reset, sizeof...(Indices)> MakeResetDetours(
    std::index_sequence<Indices...>
) {
    return {&DetourResetSlot<Indices>...};
}

static constexpr auto kResetDetours = MakeResetDetours(
    std::make_index_sequence<kMaxResetHookSlots>()
);

template<size_t HookIndex>
static void WINAPI DetourSetDescriptorHeapsSlot(
    ID3D12GraphicsCommandList *pThis,
    UINT NumDescriptorHeaps,
    ID3D12DescriptorHeap *const *ppDescriptorHeaps
);

static void DetourSetDescriptorHeapsImpl(
    size_t hookIndex,
    ID3D12GraphicsCommandList *pThis,
    UINT NumDescriptorHeaps,
    ID3D12DescriptorHeap *const *ppDescriptorHeaps
) {
    if (hookIndex >= s_setDescriptorHeapsHookCount) {
        LogError(std::format(
            "DetourSetDescriptorHeaps invoked with invalid hook slot {}.",
            hookIndex
        ));
        return;
    }

    const SetDescriptorHeapsHookState &hookState = s_setDescriptorHeapsHooks[hookIndex];
    if (hookState.hook == nullptr) {
        LogError(std::format(
            "DetourSetDescriptorHeaps invoked with missing hook state at slot {}.",
            hookIndex
        ));
        return;
    }

    ++s_setDescriptorHeapsObservedCount;
    for (UINT i = 0; i < NumDescriptorHeaps; ++i) {
        ID3D12DescriptorHeap *pHeap = ppDescriptorHeaps != nullptr ? ppDescriptorHeaps[i] : nullptr;
        if (IsShaderVisibleCBVSRVUAVHeap(pHeap)) {
            ID3D12Device *pDevice = g_unityGraphics_D3D12 != nullptr ? g_unityGraphics_D3D12->GetDevice() : nullptr;
            CacheDescriptorHeap(pDevice, pHeap, "SetDescriptorHeaps");
            break;
        }
    }

    hookState.hook->GetOriginalPtr()(pThis, NumDescriptorHeaps, ppDescriptorHeaps);
}

template<size_t HookIndex>
static void WINAPI DetourSetDescriptorHeapsSlot(
    ID3D12GraphicsCommandList *pThis,
    UINT NumDescriptorHeaps,
    ID3D12DescriptorHeap *const *ppDescriptorHeaps
) {
    DetourSetDescriptorHeapsImpl(HookIndex, pThis, NumDescriptorHeaps, ppDescriptorHeaps);
}

template<size_t... Indices>
static constexpr std::array<t_SetDescriptorHeaps, sizeof...(Indices)> MakeSetDescriptorHeapsDetours(
    std::index_sequence<Indices...>
) {
    return {&DetourSetDescriptorHeapsSlot<Indices>...};
}

static constexpr auto kSetDescriptorHeapsDetours = MakeSetDescriptorHeapsDetours(
    std::make_index_sequence<kMaxSetDescriptorHeapsHookSlots>()
);

static HRESULT DetourCreateDescriptorHeapImpl(
    size_t hookIndex,
    ID3D12Device *pThis,
    const D3D12_DESCRIPTOR_HEAP_DESC *pDescriptorHeapDesc,
    REFIID riid,
    void **ppvHeap
) {
    if (hookIndex >= s_createDescriptorHeapHookCount) {
        LogError(std::format(
            "DetourCreateDescriptorHeap invoked with invalid hook slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    const CreateDescriptorHeapHookState &hookState = s_createDescriptorHeapHooks[hookIndex];
    ScopedCreateDescriptorHeapDetourDepth detourDepthGuard;
    if (hookState.original == nullptr) {
        LogError(std::format(
            "DetourCreateDescriptorHeap invoked with missing original function at slot {}.",
            hookIndex
        ));
        return E_FAIL;
    }

    if (detourDepthGuard.IsReentrant()) {
        return hookState.original(pThis, pDescriptorHeapDesc, riid, ppvHeap);
    }

    D3D12_DESCRIPTOR_HEAP_DESC patchedDescriptorHeapDesc = {};
    const D3D12_DESCRIPTOR_HEAP_DESC *descriptorHeapDescForCreate = pDescriptorHeapDesc;
    DescriptorHeapMetadata metadata = {};
    if (pDescriptorHeapDesc != nullptr) {
        metadata = BuildDescriptorHeapMetadataForCreate(*pDescriptorHeapDesc, patchedDescriptorHeapDesc);
        if (metadata.pluginOwnedRange) {
            descriptorHeapDescForCreate = &patchedDescriptorHeapDesc;
            LogInfo(std::format(
                "Reserving plugin-owned bindless descriptor range: interface={}, target={}, originalCount={}, reserveCount={}, requestedCount={}",
                hookState.interfaceName != nullptr ? hookState.interfaceName : "unknown",
                AddressWithModuleToString(hookState.target),
                pDescriptorHeapDesc->NumDescriptors,
                metadata.bindlessDescriptorCount,
                patchedDescriptorHeapDesc.NumDescriptors
            ));
        }
    }

    const HRESULT result = hookState.original(pThis, descriptorHeapDescForCreate, riid, ppvHeap);
    if (FAILED(result)) {
        LogError(std::format(
            "CreateDescriptorHeap failed: slot={}, interface={}, target={}, hr=0x{:08X}",
            hookIndex,
            hookState.interfaceName != nullptr ? hookState.interfaceName : "unknown",
            AddressWithModuleToString(hookState.target),
            static_cast<uint32_t>(result)
        ));
        return result;
    }

    if (ppvHeap == nullptr || *ppvHeap == nullptr || pDescriptorHeapDesc == nullptr) {
        return result;
    }

    ID3D12DescriptorHeap *pHeap = static_cast<ID3D12DescriptorHeap *>(*ppvHeap);
    if (!IsShaderVisibleCBVSRVUAVHeap(pHeap)) {
        return result;
    }

    const D3D12_DESCRIPTOR_HEAP_DESC actualDescriptorHeapDesc = pHeap->GetDesc();
    if (!metadata.pluginOwnedRange) {
        metadata = {};
        metadata.totalDescriptorCount = actualDescriptorHeapDesc.NumDescriptors;
        metadata.bindlessDescriptorStart = actualDescriptorHeapDesc.NumDescriptors;
        metadata.bindlessDescriptorCount = 0;
        metadata.pluginOwnedRange = false;
    } else {
        NormalizeDescriptorHeapMetadata(actualDescriptorHeapDesc, metadata);
    }

    StoreDescriptorHeapMetadata(pHeap, metadata);
    CacheDescriptorHeap(pThis, pHeap, "CreateDescriptorHeap");
    return result;
}

#define DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(index) \
    static HRESULT WINAPI DetourCreateDescriptorHeap##index( \
        ID3D12Device *pThis, \
        const D3D12_DESCRIPTOR_HEAP_DESC *pDescriptorHeapDesc, \
        REFIID riid, \
        void **ppvHeap \
    ) { \
        return DetourCreateDescriptorHeapImpl(index, pThis, pDescriptorHeapDesc, riid, ppvHeap); \
    }

DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(0)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(1)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(2)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(3)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(4)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(5)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(6)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(7)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(8)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(9)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(10)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(11)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(12)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(13)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(14)
DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR(15)

#undef DEFINE_CREATE_DESCRIPTOR_HEAP_DETOUR

static const std::array<t_CreateDescriptorHeap, kMaxCreateDescriptorHeapHookSlots> kCreateDescriptorHeapDetours = {
    &DetourCreateDescriptorHeap0,
    &DetourCreateDescriptorHeap1,
    &DetourCreateDescriptorHeap2,
    &DetourCreateDescriptorHeap3,
    &DetourCreateDescriptorHeap4,
    &DetourCreateDescriptorHeap5,
    &DetourCreateDescriptorHeap6,
    &DetourCreateDescriptorHeap7,
    &DetourCreateDescriptorHeap8,
    &DetourCreateDescriptorHeap9,
    &DetourCreateDescriptorHeap10,
    &DetourCreateDescriptorHeap11,
    &DetourCreateDescriptorHeap12,
    &DetourCreateDescriptorHeap13,
    &DetourCreateDescriptorHeap14,
    &DetourCreateDescriptorHeap15
};

static bool InstallCreateDescriptorHeapVTablePatch(
    const char *interfaceName,
    void **entry,
    void *originalTarget,
    size_t hookIndex,
    std::string &diagnosticReport
) {
    if (entry == nullptr || originalTarget == nullptr || hookIndex >= s_createDescriptorHeapHookCount) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format(
                "{}=invalid-vtable-patch(entry={}, target={}, hookIndex={})",
                interfaceName != nullptr ? interfaceName : "unknown",
                PointerToString(entry),
                PointerToString(originalTarget),
                hookIndex
            )
        );
        return false;
    }

    const size_t existingPatchIndex = FindCreateDescriptorHeapVTablePatchIndexByEntry(entry);
    if (existingPatchIndex != SIZE_MAX) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format(
                "{}=vtable-duplicate(entry={}, hookSlot={})",
                interfaceName != nullptr ? interfaceName : "unknown",
                PointerToString(entry),
                s_createDescriptorHeapVTablePatches[existingPatchIndex].hookIndex
            )
        );
        return true;
    }

    if (s_createDescriptorHeapVTablePatchCount >= s_createDescriptorHeapVTablePatches.size()) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=vtable-capacity-exceeded", interfaceName != nullptr ? interfaceName : "unknown")
        );
        return false;
    }

    if (*entry != reinterpret_cast<void *>(kCreateDescriptorHeapDetours[hookIndex])) {
        if (!PatchPointerWithWritableProtection(entry, reinterpret_cast<void *>(kCreateDescriptorHeapDetours[hookIndex]))) {
            AppendDiagnosticInfo(
                diagnosticReport,
                std::format(
                    "{}=vtable-patch-failed(entry={}, target={})",
                    interfaceName != nullptr ? interfaceName : "unknown",
                    PointerToString(entry),
                    AddressWithModuleToString(originalTarget)
                )
            );
            return false;
        }
    }

    CreateDescriptorHeapVTablePatchState &patchState =
        s_createDescriptorHeapVTablePatches[s_createDescriptorHeapVTablePatchCount++];
    patchState.interfaceName = interfaceName;
    patchState.entry = entry;
    patchState.originalTarget = originalTarget;
    patchState.hookIndex = hookIndex;

    AppendDiagnosticInfo(
        diagnosticReport,
        std::format(
            "{}=vtable-patched(entry={}, target={}, hookSlot={})",
            interfaceName != nullptr ? interfaceName : "unknown",
            PointerToString(entry),
            AddressWithModuleToString(originalTarget),
            hookIndex
        )
    );
    return true;
}

static void DisableCreateDescriptorHeapHooks() {
    for (size_t i = 0; i < s_createDescriptorHeapVTablePatchCount; ++i) {
        const CreateDescriptorHeapVTablePatchState &patchState = s_createDescriptorHeapVTablePatches[i];
        if (patchState.entry == nullptr || patchState.originalTarget == nullptr) {
            continue;
        }

        if (!PatchPointerWithWritableProtection(patchState.entry, patchState.originalTarget)) {
            LogError(std::format(
                "Failed to restore CreateDescriptorHeap vtable patch for {} at entry={}.",
                patchState.interfaceName != nullptr ? patchState.interfaceName : "unknown",
                PointerToString(patchState.entry)
            ));
        }
    }

    s_createDescriptorHeapVTablePatchCount = 0;
    s_createDescriptorHeapVTablePatches = {};

    for (size_t i = 0; i < s_createDescriptorHeapHookCount; ++i) {
        CreateDescriptorHeapHookState &hookState = s_createDescriptorHeapHooks[i];
        if (hookState.hook == nullptr) {
            hookState = {};
            continue;
        }

        hookState.hook->Disable();
        delete hookState.hook;
        hookState = {};
    }

    s_createDescriptorHeapHookCount = 0;
}

static void DisableSetDescriptorHeapsHooks() {
    for (size_t i = 0; i < s_setDescriptorHeapsHookCount; ++i) {
        SetDescriptorHeapsHookState &hookState = s_setDescriptorHeapsHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        hookState.hook->Disable();
        delete hookState.hook;
        hookState = {};
    }

    s_setDescriptorHeapsHookCount = 0;
    s_setDescriptorHeapsHookReport = "not attempted";
}

static void DisableResetHooks() {
    for (size_t i = 0; i < s_resetHookCount; ++i) {
        ResetHookState &hookState = s_resetHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        hookState.hook->Disable();
        delete hookState.hook;
        hookState = {};
    }

    s_resetHookCount = 0;
    s_resetHookReport = "not attempted";
}

static void DisableCreateCommandListHooks() {
    for (size_t i = 0; i < s_createCommandListHookCount; ++i) {
        CreateCommandListHookState &hookState = s_createCommandListHooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        hookState.hook->Disable();
        delete hookState.hook;
        hookState = {};
    }

    for (size_t i = 0; i < s_createCommandList1HookCount; ++i) {
        CreateCommandList1HookState &hookState = s_createCommandList1Hooks[i];
        if (hookState.hook == nullptr) {
            continue;
        }

        hookState.hook->Disable();
        delete hookState.hook;
        hookState = {};
    }

    s_createCommandListHookCount = 0;
    s_createCommandList1HookCount = 0;
    s_createCommandListHookReport = "not attempted";
    s_createCommandList1HookReport = "not attempted";
}

template<typename TDeviceInterface>
static void TryInstallCreateCommandListHookForInterface(
    ID3D12Device *pDevice,
    const char *interfaceName,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    Microsoft::WRL::ComPtr<TDeviceInterface> pInterface;
    const HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(pInterface.GetAddressOf()));
    if (FAILED(hr) || pInterface == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hr=0x{:08X}", interfaceName, static_cast<uint32_t>(hr))
        );
        return;
    }

    void **pDeviceVTable = *reinterpret_cast<void ***>(pInterface.Get());
    void *fnCreateCommandList = pDeviceVTable[kCreateCommandListVTableIndex];
    if (fnCreateCommandList == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=null-target", interfaceName)
        );
        return;
    }

    if (!seenTargets.insert(fnCreateCommandList).second) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=duplicate({})", interfaceName, AddressWithModuleToString(fnCreateCommandList))
        );
        return;
    }

    if (s_createCommandListHookCount >= s_createCommandListHooks.size()) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=capacity-exceeded", interfaceName)
        );
        return;
    }

    const size_t hookIndex = s_createCommandListHookCount;
    CreateCommandListHookState &hookState = s_createCommandListHooks[hookIndex];
    hookState.hook = new HookWrapper<t_CreateCommandList>(fnCreateCommandList);
    hookState.interfaceName = interfaceName;
    hookState.target = fnCreateCommandList;
    s_createCommandListHookCount = hookIndex + 1;
    if (!hookState.hook->CreateAndEnable(kCreateCommandListDetours[hookIndex])) {
        delete hookState.hook;
        hookState = {};
        s_createCommandListHookCount = hookIndex;
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hook-failed({})", interfaceName, AddressWithModuleToString(fnCreateCommandList))
        );
        return;
    }

    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("{}={}", interfaceName, AddressWithModuleToString(fnCreateCommandList))
    );
}

template<typename TDeviceInterface>
static void TryInstallCreateCommandList1HookForInterface(
    ID3D12Device *pDevice,
    const char *interfaceName,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    Microsoft::WRL::ComPtr<TDeviceInterface> pInterface;
    const HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(pInterface.GetAddressOf()));
    if (FAILED(hr) || pInterface == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hr=0x{:08X}", interfaceName, static_cast<uint32_t>(hr))
        );
        return;
    }

    void **pDeviceVTable = *reinterpret_cast<void ***>(pInterface.Get());
    void *fnCreateCommandList1 = pDeviceVTable[kCreateCommandList1VTableIndex];
    if (fnCreateCommandList1 == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=null-target", interfaceName)
        );
        return;
    }

    if (!seenTargets.insert(fnCreateCommandList1).second) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=duplicate({})", interfaceName, AddressWithModuleToString(fnCreateCommandList1))
        );
        return;
    }

    if (s_createCommandList1HookCount >= s_createCommandList1Hooks.size()) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=capacity-exceeded", interfaceName)
        );
        return;
    }

    const size_t hookIndex = s_createCommandList1HookCount;
    CreateCommandList1HookState &hookState = s_createCommandList1Hooks[hookIndex];
    hookState.hook = new HookWrapper<t_CreateCommandList1>(fnCreateCommandList1);
    hookState.interfaceName = interfaceName;
    hookState.target = fnCreateCommandList1;
    s_createCommandList1HookCount = hookIndex + 1;
    if (!hookState.hook->CreateAndEnable(kCreateCommandList1Detours[hookIndex])) {
        delete hookState.hook;
        hookState = {};
        s_createCommandList1HookCount = hookIndex;
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hook-failed({})", interfaceName, AddressWithModuleToString(fnCreateCommandList1))
        );
        return;
    }

    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("{}={}", interfaceName, AddressWithModuleToString(fnCreateCommandList1))
    );
}

#define TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(interfaceType) \
    TryInstallCreateCommandListHookForInterface<interfaceType>( \
        pDevice, \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    )

static bool InstallCreateCommandListHooks(ID3D12Device *pDevice) {
    if (s_createCommandListHookCount > 0) {
        return true;
    }

    if (pDevice == nullptr) {
        LogError("Cannot install CreateCommandList hooks because D3D12 device is null.");
        return false;
    }

    std::unordered_set<void *> seenTargets;
    std::string diagnosticReport;
    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("d3d12={}", ModuleHandleToString(GetModuleHandleW(L"d3d12.dll")))
    );

    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device);
#ifdef __ID3D12Device1_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device1);
#endif
#ifdef __ID3D12Device2_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device2);
#endif
#ifdef __ID3D12Device3_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device3);
#endif
#ifdef __ID3D12Device4_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device4);
#endif
#ifdef __ID3D12Device5_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device5);
#endif
#ifdef __ID3D12Device6_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device6);
#endif
#ifdef __ID3D12Device7_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device7);
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device8);
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device9);
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device10);
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device11);
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device12);
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device13);
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device14);
#endif
#ifdef __ID3D12Device15_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST_HOOK(ID3D12Device15);
#endif

    s_createCommandListHookReport = diagnosticReport.empty() ? "none" : diagnosticReport;
    if (s_createCommandListHookCount == 0) {
        LogError(std::format(
            "Failed to install CreateCommandList hooks. report={}",
            s_createCommandListHookReport
        ));
        return false;
    }

    LogInfo(std::format(
        "Hooked CreateCommandList: {}",
        BuildCreateCommandListHookInfo()
    ));
    return true;
}

#undef TRY_INSTALL_CREATE_COMMAND_LIST_HOOK

#define TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(interfaceType) \
    TryInstallCreateCommandList1HookForInterface<interfaceType>( \
        pDevice, \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    )

static bool InstallCreateCommandList1Hooks(ID3D12Device *pDevice) {
    if (s_createCommandList1HookCount > 0) {
        return true;
    }

    if (pDevice == nullptr) {
        LogError("Cannot install CreateCommandList1 hooks because D3D12 device is null.");
        return false;
    }

    std::unordered_set<void *> seenTargets;
    std::string diagnosticReport;
    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("d3d12={}", ModuleHandleToString(GetModuleHandleW(L"d3d12.dll")))
    );

#ifdef __ID3D12Device4_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device4);
#endif
#ifdef __ID3D12Device5_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device5);
#endif
#ifdef __ID3D12Device6_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device6);
#endif
#ifdef __ID3D12Device7_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device7);
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device8);
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device9);
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device10);
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device11);
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device12);
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device13);
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device14);
#endif
#ifdef __ID3D12Device15_INTERFACE_DEFINED__
    TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK(ID3D12Device15);
#endif

    s_createCommandList1HookReport = diagnosticReport.empty() ? "none" : diagnosticReport;
    if (s_createCommandList1HookCount == 0) {
        LogInfo(std::format(
            "CreateCommandList1 hooks were not installed. report={}",
            s_createCommandList1HookReport
        ));
        return true;
    }

    LogInfo(std::format(
        "Hooked CreateCommandList1: {}",
        BuildCreateCommandList1HookInfo()
    ));
    return true;
}

#undef TRY_INSTALL_CREATE_COMMAND_LIST1_HOOK

template<typename TCommandListInterface>
static void TryInstallResetHookForCommandListObject(
    TCommandListInterface *pCommandList,
    const char *label,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    if (pCommandList == nullptr) {
        AppendDiagnosticInfo(diagnosticReport, std::format("{}=null-command-list", label));
        return;
    }

    void **pCommandListVTable = *reinterpret_cast<void ***>(pCommandList);
    void *fnReset = pCommandListVTable[kResetVTableIndex];
    if (fnReset == nullptr) {
        AppendDiagnosticInfo(diagnosticReport, std::format("{}=null-target", label));
        return;
    }

    if (!seenTargets.insert(fnReset).second) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=duplicate({})", label, AddressWithModuleToString(fnReset))
        );
        return;
    }

    if (s_resetHookCount >= s_resetHooks.size()) {
        AppendDiagnosticInfo(diagnosticReport, std::format("{}=capacity-exceeded", label));
        return;
    }

    const size_t hookIndex = s_resetHookCount;
    ResetHookState &hookState = s_resetHooks[hookIndex];
    hookState.hook = new HookWrapper<t_Reset>(fnReset);
    hookState.label = label;
    hookState.target = fnReset;
    s_resetHookCount = hookIndex + 1;
    if (!hookState.hook->CreateAndEnable(kResetDetours[hookIndex])) {
        delete hookState.hook;
        hookState = {};
        s_resetHookCount = hookIndex;
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hook-failed({})", label, AddressWithModuleToString(fnReset))
        );
        return;
    }

    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("{}={}", label, AddressWithModuleToString(fnReset))
    );
}

template<typename TCommandListInterface>
static void TryInstallResetHookViaCreateCommandList(
    ID3D12Device *pDevice,
    ID3D12CommandAllocator *pCommandAllocator,
    const char *creationMethod,
    const char *interfaceName,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    Microsoft::WRL::ComPtr<TCommandListInterface> pCommandList;
    const HRESULT hr = pDevice->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        pCommandAllocator,
        nullptr,
        IID_PPV_ARGS(pCommandList.GetAddressOf())
    );
    const std::string label = std::format(
        "{}:DIRECT:{}",
        creationMethod,
        interfaceName
    );
    if (FAILED(hr) || pCommandList == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hr=0x{:08X}", label, static_cast<uint32_t>(hr))
        );
        return;
    }

    TryInstallResetHookForCommandListObject(
        pCommandList.Get(),
        label.c_str(),
        seenTargets,
        diagnosticReport
    );
    pCommandList->Close();
}

#define TRY_INSTALL_RESET_HOOK(interfaceType) \
    TryInstallResetHookViaCreateCommandList<interfaceType>( \
        pDevice, \
        pCommandAllocator.Get(), \
        "CreateCommandList", \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    )

static bool InstallResetHooks(ID3D12Device *pDevice) {
    if (s_resetHookCount > 0) {
        return true;
    }

    if (pDevice == nullptr) {
        LogError("Cannot install Reset hooks because D3D12 device is null.");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> pCommandAllocator;
    const HRESULT hr = pDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(pCommandAllocator.GetAddressOf())
    );
    if (FAILED(hr)) {
        LogError(std::format(
            "Failed to create direct command allocator for Reset hooks: hr=0x{:08X}",
            static_cast<uint32_t>(hr)
        ));
        return false;
    }

    std::unordered_set<void *> seenTargets;
    std::string diagnosticReport;
    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("d3d12={}", ModuleHandleToString(GetModuleHandleW(L"d3d12.dll")))
    );

    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList);
#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList1);
#endif
#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList2);
#endif
#ifdef __ID3D12GraphicsCommandList3_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList3);
#endif
#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList4);
#endif
#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList5);
#endif
#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList6);
#endif
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList7);
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList8);
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList9);
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    TRY_INSTALL_RESET_HOOK(ID3D12GraphicsCommandList10);
#endif

    s_resetHookReport = diagnosticReport.empty() ? "none" : diagnosticReport;
    if (s_resetHookCount == 0) {
        LogError(std::format(
            "Failed to install Reset hooks. report={}",
            s_resetHookReport
        ));
        return false;
    }

    LogInfo(std::format(
        "Hooked Reset: {}",
        BuildResetHookInfo()
    ));
    return true;
}

#undef TRY_INSTALL_RESET_HOOK

template<typename TDeviceInterface>
static void TryInstallCreateDescriptorHeapHookForInterface(
    ID3D12Device *pDevice,
    const char *interfaceName,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    Microsoft::WRL::ComPtr<TDeviceInterface> pInterface;
    const HRESULT hr = pDevice->QueryInterface(IID_PPV_ARGS(pInterface.GetAddressOf()));
    if (FAILED(hr) || pInterface == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hr=0x{:08X}", interfaceName, static_cast<uint32_t>(hr))
        );
        return;
    }

    void **pDeviceVTable = *reinterpret_cast<void ***>(pInterface.Get());
    void **pCreateDescriptorHeapEntry = &pDeviceVTable[kCreateDescriptorHeapVTableIndex];
    void *fnCreateDescriptorHeap = pDeviceVTable[kCreateDescriptorHeapVTableIndex];
    if (fnCreateDescriptorHeap == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=null-target", interfaceName)
        );
        return;
    }

    size_t hookIndex = FindCreateDescriptorHeapHookIndexByTarget(fnCreateDescriptorHeap);
    if (hookIndex == SIZE_MAX) {
        if (s_createDescriptorHeapHookCount >= s_createDescriptorHeapHooks.size()) {
            AppendDiagnosticInfo(
                diagnosticReport,
                std::format("{}=capacity-exceeded", interfaceName)
            );
            return;
        }

        hookIndex = s_createDescriptorHeapHookCount;
        CreateDescriptorHeapHookState &hookState = s_createDescriptorHeapHooks[hookIndex];
        hookState.interfaceName = interfaceName;
        hookState.target = fnCreateDescriptorHeap;
        hookState.original = reinterpret_cast<t_CreateDescriptorHeap>(fnCreateDescriptorHeap);
        s_createDescriptorHeapHookCount = hookIndex + 1;

        if (seenTargets.insert(fnCreateDescriptorHeap).second) {
            hookState.hook = new HookWrapper<t_CreateDescriptorHeap>(fnCreateDescriptorHeap);
            if (!hookState.hook->CreateAndEnable(kCreateDescriptorHeapDetours[hookIndex])) {
                delete hookState.hook;
                hookState.hook = nullptr;
                AppendDiagnosticInfo(
                    diagnosticReport,
                    std::format("{}=minhook-failed({})", interfaceName, AddressWithModuleToString(fnCreateDescriptorHeap))
                );
            } else {
                hookState.original = hookState.hook->GetOriginalPtr();
                AppendDiagnosticInfo(
                    diagnosticReport,
                    std::format("{}=minhook({})", interfaceName, AddressWithModuleToString(fnCreateDescriptorHeap))
                );
            }
        }
    }

    CreateDescriptorHeapHookState &hookState = s_createDescriptorHeapHooks[hookIndex];
    if (hookState.original == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=missing-original({})", interfaceName, AddressWithModuleToString(fnCreateDescriptorHeap))
        );
        return;
    }

    InstallCreateDescriptorHeapVTablePatch(
        interfaceName,
        pCreateDescriptorHeapEntry,
        fnCreateDescriptorHeap,
        hookIndex,
        diagnosticReport
    );
}

static bool InstallCreateDescriptorHeapHooks(ID3D12Device *pDevice) {
    if (s_createDescriptorHeapHookCount > 0) {
        return true;
    }

    if (pDevice == nullptr) {
        LogError("Cannot install CreateDescriptorHeap hooks because D3D12 device is null.");
        return false;
    }

    std::unordered_set<void *> seenTargets;
    std::string diagnosticReport;
    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("d3d12={}", ModuleHandleToString(GetModuleHandleW(L"d3d12.dll")))
    );
    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("d3d12core={}", ModuleHandleToString(GetModuleHandleW(L"D3D12Core.dll")))
    );

    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device>(pDevice, "ID3D12Device", seenTargets, diagnosticReport);
#ifdef __ID3D12Device1_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device1>(pDevice, "ID3D12Device1", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device2_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device2>(pDevice, "ID3D12Device2", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device3_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device3>(pDevice, "ID3D12Device3", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device4_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device4>(pDevice, "ID3D12Device4", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device5_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device5>(pDevice, "ID3D12Device5", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device6_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device6>(pDevice, "ID3D12Device6", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device7_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device7>(pDevice, "ID3D12Device7", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device8>(pDevice, "ID3D12Device8", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device9>(pDevice, "ID3D12Device9", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device10>(pDevice, "ID3D12Device10", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device11>(pDevice, "ID3D12Device11", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device12>(pDevice, "ID3D12Device12", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device13>(pDevice, "ID3D12Device13", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device14>(pDevice, "ID3D12Device14", seenTargets, diagnosticReport);
#endif
#ifdef __ID3D12Device15_INTERFACE_DEFINED__
    TryInstallCreateDescriptorHeapHookForInterface<ID3D12Device15>(pDevice, "ID3D12Device15", seenTargets, diagnosticReport);
#endif

    s_createDescriptorHeapHookReport = diagnosticReport.empty() ? "none" : diagnosticReport;
    if (s_createDescriptorHeapHookCount == 0) {
        LogError(std::format(
            "Failed to install CreateDescriptorHeap hooks. report={}",
            s_createDescriptorHeapHookReport
        ));
        return false;
    }

    LogInfo(std::format(
        "Hooked CreateDescriptorHeap: {}",
        BuildCreateDescriptorHeapHookInfo()
    ));
    return true;
}

template<typename TCommandListInterface>
static void TryInstallSetDescriptorHeapsHookForCommandListObject(
    TCommandListInterface *pCommandList,
    const char *label,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    if (pCommandList == nullptr) {
        AppendDiagnosticInfo(diagnosticReport, std::format("{}=null-command-list", label));
        return;
    }

    void **pCommandListVTable = *reinterpret_cast<void ***>(pCommandList);
    void *fnSetDescriptorHeaps = pCommandListVTable[kSetDescriptorHeapsVTableIndex];
    if (fnSetDescriptorHeaps == nullptr) {
        AppendDiagnosticInfo(diagnosticReport, std::format("{}=null-target", label));
        return;
    }

    if (!seenTargets.insert(fnSetDescriptorHeaps).second) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=duplicate({})", label, AddressWithModuleToString(fnSetDescriptorHeaps))
        );
        return;
    }

    if (s_setDescriptorHeapsHookCount >= s_setDescriptorHeapsHooks.size()) {
        AppendDiagnosticInfo(diagnosticReport, std::format("{}=capacity-exceeded", label));
        return;
    }

    const size_t hookIndex = s_setDescriptorHeapsHookCount;
    SetDescriptorHeapsHookState &hookState = s_setDescriptorHeapsHooks[hookIndex];
    hookState.hook = new HookWrapper<t_SetDescriptorHeaps>(fnSetDescriptorHeaps);
    hookState.label = label;
    hookState.target = fnSetDescriptorHeaps;
    s_setDescriptorHeapsHookCount = hookIndex + 1;
    if (!hookState.hook->CreateAndEnable(kSetDescriptorHeapsDetours[hookIndex])) {
        delete hookState.hook;
        hookState = {};
        s_setDescriptorHeapsHookCount = hookIndex;
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hook-failed({})", label, AddressWithModuleToString(fnSetDescriptorHeaps))
        );
        return;
    }

    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("{}={}", label, AddressWithModuleToString(fnSetDescriptorHeaps))
    );
}

template<typename TCommandListInterface>
static void TryInstallSetDescriptorHeapsHookViaCreateCommandList(
    ID3D12Device *pDevice,
    ID3D12CommandAllocator *pCommandAllocator,
    D3D12_COMMAND_LIST_TYPE commandListType,
    const char *creationMethod,
    const char *interfaceName,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    Microsoft::WRL::ComPtr<TCommandListInterface> pCommandList;
    const HRESULT hr = pDevice->CreateCommandList(
        0,
        commandListType,
        pCommandAllocator,
        nullptr,
        IID_PPV_ARGS(pCommandList.GetAddressOf())
    );
    const std::string label = std::format(
        "{}:{}:{}",
        creationMethod,
        commandListType == D3D12_COMMAND_LIST_TYPE_BUNDLE ? "BUNDLE" : "DIRECT",
        interfaceName
    );
    if (FAILED(hr) || pCommandList == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hr=0x{:08X}", label, static_cast<uint32_t>(hr))
        );
        return;
    }

    TryInstallSetDescriptorHeapsHookForCommandListObject(
        pCommandList.Get(),
        label.c_str(),
        seenTargets,
        diagnosticReport
    );
    pCommandList->Close();
}

template<typename TCommandListInterface>
static void TryInstallSetDescriptorHeapsHookViaCreateCommandList1(
    ID3D12Device4 *pDevice,
    D3D12_COMMAND_LIST_TYPE commandListType,
    const char *creationMethod,
    const char *interfaceName,
    std::unordered_set<void *> &seenTargets,
    std::string &diagnosticReport
) {
    Microsoft::WRL::ComPtr<TCommandListInterface> pCommandList;
    const HRESULT hr = pDevice->CreateCommandList1(
        0,
        commandListType,
        D3D12_COMMAND_LIST_FLAG_NONE,
        IID_PPV_ARGS(pCommandList.GetAddressOf())
    );
    const std::string label = std::format(
        "{}:{}:{}",
        creationMethod,
        commandListType == D3D12_COMMAND_LIST_TYPE_BUNDLE ? "BUNDLE" : "DIRECT",
        interfaceName
    );
    if (FAILED(hr) || pCommandList == nullptr) {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("{}=hr=0x{:08X}", label, static_cast<uint32_t>(hr))
        );
        return;
    }

    TryInstallSetDescriptorHeapsHookForCommandListObject(
        pCommandList.Get(),
        label.c_str(),
        seenTargets,
        diagnosticReport
    );
    pCommandList->Close();
}
#define TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(interfaceType) \
    TryInstallSetDescriptorHeapsHookViaCreateCommandList<interfaceType>( \
        pDevice, \
        pDirectCommandAllocator.Get(), \
        D3D12_COMMAND_LIST_TYPE_DIRECT, \
        "CreateCommandList", \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    ); \
    TryInstallSetDescriptorHeapsHookViaCreateCommandList<interfaceType>( \
        pDevice, \
        pBundleCommandAllocator.Get(), \
        D3D12_COMMAND_LIST_TYPE_BUNDLE, \
        "CreateCommandList", \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    )

#define TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(interfaceType) \
    TryInstallSetDescriptorHeapsHookViaCreateCommandList1<interfaceType>( \
        pDevice4.Get(), \
        D3D12_COMMAND_LIST_TYPE_DIRECT, \
        "CreateCommandList1", \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    ); \
    TryInstallSetDescriptorHeapsHookViaCreateCommandList1<interfaceType>( \
        pDevice4.Get(), \
        D3D12_COMMAND_LIST_TYPE_BUNDLE, \
        "CreateCommandList1", \
        #interfaceType, \
        seenTargets, \
        diagnosticReport \
    )

static bool InstallSetDescriptorHeapsHooks(ID3D12Device *pDevice) {
    if (s_setDescriptorHeapsHookCount > 0) {
        return true;
    }

    if (pDevice == nullptr) {
        LogError("Cannot install SetDescriptorHeaps hooks because D3D12 device is null.");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> pDirectCommandAllocator;
    HRESULT hr = pDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(pDirectCommandAllocator.GetAddressOf())
    );
    if (FAILED(hr)) {
        LogError(std::format(
            "Failed to create direct command allocator for SetDescriptorHeaps hooks: hr=0x{:08X}",
            static_cast<uint32_t>(hr)
        ));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> pBundleCommandAllocator;
    hr = pDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_BUNDLE,
        IID_PPV_ARGS(pBundleCommandAllocator.GetAddressOf())
    );
    if (FAILED(hr)) {
        LogError(std::format(
            "Failed to create bundle command allocator for SetDescriptorHeaps hooks: hr=0x{:08X}",
            static_cast<uint32_t>(hr)
        ));
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device4> pDevice4;
    const HRESULT device4Result = pDevice->QueryInterface(IID_PPV_ARGS(pDevice4.GetAddressOf()));

    std::unordered_set<void *> seenTargets;
    std::string diagnosticReport;
    AppendDiagnosticInfo(
        diagnosticReport,
        std::format("d3d12={}", ModuleHandleToString(GetModuleHandleW(L"d3d12.dll")))
    );

    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList);
#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList1);
#endif
#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList2);
#endif
#ifdef __ID3D12GraphicsCommandList3_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList3);
#endif
#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList4);
#endif
#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList5);
#endif
#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList6);
#endif
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList7);
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList8);
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList9);
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
    TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST(ID3D12GraphicsCommandList10);
#endif

    if (SUCCEEDED(device4Result) && pDevice4 != nullptr) {
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList);
#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList1);
#endif
#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList2);
#endif
#ifdef __ID3D12GraphicsCommandList3_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList3);
#endif
#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList4);
#endif
#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList5);
#endif
#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList6);
#endif
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList7);
#endif
#ifdef __ID3D12GraphicsCommandList8_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList8);
#endif
#ifdef __ID3D12GraphicsCommandList9_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList9);
#endif
#ifdef __ID3D12GraphicsCommandList10_INTERFACE_DEFINED__
        TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1(ID3D12GraphicsCommandList10);
#endif
    } else {
        AppendDiagnosticInfo(
            diagnosticReport,
            std::format("ID3D12Device4=hr=0x{:08X}", static_cast<uint32_t>(device4Result))
        );
    }

    s_setDescriptorHeapsHookReport = diagnosticReport.empty() ? "none" : diagnosticReport;
    if (s_setDescriptorHeapsHookCount == 0) {
        LogError(std::format(
            "Failed to install SetDescriptorHeaps hooks. report={}",
            s_setDescriptorHeapsHookReport
        ));
        return false;
    }

    LogInfo(std::format(
        "Hooked SetDescriptorHeaps: {}",
        BuildSetDescriptorHeapsHookInfo()
    ));
    return true;
}

#undef TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST
#undef TRY_INSTALL_SET_DESCRIPTOR_HEAPS_CREATE_COMMAND_LIST1

#define NAMEOF(x) #x

static FARPROC GetD3D12SerializeRootSignatureTargetFunction() {
    return GetProcAddress(GetModuleHandleA("d3d12.dll"), NAMEOF(D3D12SerializeRootSignature));
}

static bool InstallRootSignatureHook() {
    if (s_pSerializeRootSignatureHook != nullptr) {
        return true;
    }

    FARPROC pTarget = GetD3D12SerializeRootSignatureTargetFunction();
    if (pTarget == nullptr) {
        LogError("Failed to resolve D3D12SerializeRootSignature.");
        return false;
    }

    s_pSerializeRootSignatureHook = new HookWrapper<t_D3D12SerializeRootSignature>(
        reinterpret_cast<LPVOID>(pTarget)
    );
    if (!s_pSerializeRootSignatureHook->CreateAndEnable(&DetourD3D12SerializeRootSignature)) {
        delete s_pSerializeRootSignatureHook;
        s_pSerializeRootSignatureHook = nullptr;
        LogError("Failed to hook D3D12SerializeRootSignature.");
        return false;
    }

    LogInfo("Hooked D3D12SerializeRootSignature.");
    return true;
}

static void UninstallRootSignatureHook() {
    if (s_pSerializeRootSignatureHook == nullptr) {
        return;
    }

    s_pSerializeRootSignatureHook->Disable();
    delete s_pSerializeRootSignatureHook;
    s_pSerializeRootSignatureHook = nullptr;
}

static void InitializeD3D12HooksIfPossible() {
    if (g_unityInterfaces == nullptr) {
        return;
    }

    IUnityGraphicsD3D12v8 *pD3d12 = g_unityInterfaces->Get<IUnityGraphicsD3D12v8>();
    g_unityGraphics_D3D12 = pD3d12;
    if (pD3d12 == nullptr) {
        LogError("IUnityGraphicsD3D12v8 interface is null during initialize.");
        return;
    }

    ID3D12Device *pDevice = pD3d12->GetDevice();
    if (pDevice == nullptr) {
        LogError("Failed to resolve D3D12 device during initialize.");
        return;
    }

    if (!InstallCreateDescriptorHeapHooks(pDevice)) {
        LogError("Failed to install any CreateDescriptorHeap hooks.");
    }
    if (!InstallSetDescriptorHeapsHooks(pDevice)) {
        LogError("Failed to install SetDescriptorHeaps hooks.");
    }
    if (!InstallResetHooks(pDevice)) {
        LogError("Failed to install Reset hooks.");
    }
    if (!InstallCreateCommandListHooks(pDevice)) {
        LogError("Failed to install CreateCommandList hooks.");
    }
    if (!InstallCreateCommandList1Hooks(pDevice)) {
        LogError("Failed to install CreateCommandList1 hooks.");
    }
}
extern "C" {
//-------------------------------------------------------
// Unity interfaces

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType) {
    switch (eventType) {
        case kUnityGfxDeviceEventInitialize:
            InitializeD3D12HooksIfPossible();
            break;
        case kUnityGfxDeviceEventBeforeReset:
            DisableCreateCommandListHooks();
            DisableCreateDescriptorHeapHooks();
            DisableResetHooks();
            DisableSetDescriptorHeapsHooks();
            ResetDescriptorHeapTrackingState();
            break;
        case kUnityGfxDeviceEventAfterReset:
            InitializeD3D12HooksIfPossible();
            break;
        case kUnityGfxDeviceEventShutdown:
            DisableCreateCommandListHooks();
            DisableCreateDescriptorHeapHooks();
            DisableResetHooks();
            DisableSetDescriptorHeapsHooks();
            ResetDescriptorHeapTrackingState();
            s_createCommandListHookReport = "not attempted";
            s_createCommandList1HookReport = "not attempted";
            s_createDescriptorHeapHookReport = "not attempted";
            s_resetHookReport = "not attempted";
            break;
        default:
            break;
    }
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces *unityInterfaces) {
    g_unityInterfaces = unityInterfaces;
    g_Log = g_unityInterfaces->Get<IUnityLog>();
    g_unityGraphics = g_unityInterfaces->Get<IUnityGraphics>();
    LogInfo(std::format(
        "UnityBindless plugin loaded. renderer={}",
        RendererToString(g_unityGraphics != nullptr ? g_unityGraphics->GetRenderer() : kUnityGfxRendererNull)
    ));

    const HMODULE d3d12Module = LoadLibraryW(L"d3d12.dll");
    if (d3d12Module == nullptr) {
        LogError("Failed to load d3d12.dll.");
    }

    if (g_unityGraphics != nullptr) {
        g_unityGraphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }

    if (MH_Initialize() == MH_OK) {
        LogInfo("MinHook initialized.");
    } else {
        LogError("MH_Initialize failure");
    }

    InstallRootSignatureHook();

    g_unityGraphics_D3D12 = g_unityInterfaces->Get<IUnityGraphicsD3D12v8>();
    if (g_unityGraphics != nullptr && g_unityGraphics->GetRenderer() == kUnityGfxRendererD3D12) {
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    }
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload() {
    if (g_unityGraphics != nullptr) {
        g_unityGraphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }

    DisableCreateCommandListHooks();
    DisableCreateDescriptorHeapHooks();
    DisableResetHooks();
    DisableSetDescriptorHeapsHooks();
    ResetDescriptorHeapTrackingState();
    UninstallRootSignatureHook();

    if (MH_Uninitialize() == MH_OK) {
        LogInfo("MinHook uninitialized.");
    } else {
        LogError("MH_Uninitialize failure");
    }

    g_unityGraphics_D3D12 = nullptr;
    g_unityGraphics = nullptr;
    g_Log = nullptr;
    s_createCommandListHookReport = "not attempted";
    s_createCommandList1HookReport = "not attempted";
    s_createDescriptorHeapHookReport = "not attempted";
    s_resetHookReport = "not attempted";
}

UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API WarmupPlugin() {
    return 1u;
}

UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API GetSRVDescriptorHeapCount() {
    const DescriptorHeapSelectionState *selection = GetDescriptorHeapSelectionForQueries();
    if (selection == nullptr) {
        LogDescriptorHeapUnavailable("Failed to get descriptor heap");
        return 0u;
    }

    return selection->totalDescriptorCount;
}

UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API GetBindlessDescriptorStartIndex() {
    const DescriptorHeapSelectionState *selection = GetBindlessDescriptorHeapSelection();
    return selection != nullptr ? selection->bindlessDescriptorStart : 0u;
}

UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API GetBindlessDescriptorCount() {
    const DescriptorHeapSelectionState *selection = GetBindlessDescriptorHeapSelection();
    return selection != nullptr ? selection->bindlessDescriptorCount : 0u;
}

UNITY_INTERFACE_EXPORT bool UNITY_INTERFACE_API CreateSRVDescriptor(ID3D12Resource *pTexture, uint32_t index) {
    const DescriptorHeapSelectionState *selection = GetBindlessDescriptorHeapSelection();
    if (selection == nullptr) {
        LogDescriptorHeapUnavailable("CreateSRVDescriptor failed because no plugin-owned bindless range is available");
        return false;
    }

    if (pTexture == nullptr) {
        LogError("CreateSRVDescriptor failed because the texture resource is null.");
        return false;
    }

    if (!IsBindlessDescriptorIndexInRange(index)) {
        LogError(std::format(
            "CreateSRVDescriptor failed because index {} is outside the bindless range [{}, {}).",
            index,
            selection->bindlessDescriptorStart,
            selection->bindlessDescriptorStart + selection->bindlessDescriptorCount
        ));
        return false;
    }

    const IUnityGraphicsD3D12v8 *pD3d12 = g_unityInterfaces != nullptr ? g_unityInterfaces->Get<IUnityGraphicsD3D12v8>() : nullptr;
    ID3D12Device *pDevice = pD3d12 != nullptr ? pD3d12->GetDevice() : nullptr;
    if (pDevice == nullptr) {
        LogError("CreateSRVDescriptor failed to get D3D12 device.");
        return false;
    }

    const UINT incrementSize = selection->incrementSize != 0
        ? selection->incrementSize
        : pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (selection->cpuDescriptorHandleForHeapStart == 0 || incrementSize == 0) {
        LogDescriptorHeapUnavailable("CreateSRVDescriptor failed because the selected descriptor heap has invalid CPU start or increment size");
        return false;
    }

    const SIZE_T ptrOffset = static_cast<SIZE_T>(index) * incrementSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = {
        selection->cpuDescriptorHandleForHeapStart + ptrOffset
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = pTexture->GetDesc().Format;
    switch (srvDesc.Format) {
        case DXGI_FORMAT_D16_UNORM:
            srvDesc.Format = DXGI_FORMAT_R16_UNORM;
            break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case DXGI_FORMAT_D32_FLOAT:
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            break;
        default:
            break;
    }

    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = UINT_MAX;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    pDevice->CreateShaderResourceView(pTexture, &srvDesc, descriptorHandle);

    return true;
}

UNITY_INTERFACE_EXPORT bool UNITY_INTERFACE_API CreateUAVDescriptor(ID3D12Resource *pTexture, uint32_t index) {
    const DescriptorHeapSelectionState *selection = GetBindlessDescriptorHeapSelection();
    if (selection == nullptr) {
        LogDescriptorHeapUnavailable("CreateUAVDescriptor failed because no plugin-owned bindless range is available");
        return false;
    }

    if (pTexture == nullptr) {
        LogError("CreateUAVDescriptor failed because the texture resource is null.");
        return false;
    }

    if (!IsBindlessDescriptorIndexInRange(index)) {
        LogError(std::format(
            "CreateUAVDescriptor failed because index {} is outside the bindless range [{}, {}).",
            index,
            selection->bindlessDescriptorStart,
            selection->bindlessDescriptorStart + selection->bindlessDescriptorCount
        ));
        return false;
    }

    const IUnityGraphicsD3D12v8 *pD3d12 = g_unityInterfaces != nullptr ? g_unityInterfaces->Get<IUnityGraphicsD3D12v8>() : nullptr;
    ID3D12Device *pDevice = pD3d12 != nullptr ? pD3d12->GetDevice() : nullptr;
    if (pDevice == nullptr) {
        LogError("CreateUAVDescriptor failed to get D3D12 device.");
        return false;
    }

    const UINT incrementSize = selection->incrementSize != 0
        ? selection->incrementSize
        : pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (selection->cpuDescriptorHandleForHeapStart == 0 || incrementSize == 0) {
        LogDescriptorHeapUnavailable("CreateUAVDescriptor failed because the selected descriptor heap has invalid CPU start or increment size");
        return false;
    }

    const SIZE_T ptrOffset = static_cast<SIZE_T>(index) * incrementSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle = {
        selection->cpuDescriptorHandleForHeapStart + ptrOffset
    };

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = pTexture->GetDesc().Format;
    switch (uavDesc.Format) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            uavDesc.Format = DXGI_FORMAT_R32_UINT;
            break;
        case DXGI_FORMAT_R16_TYPELESS:
            uavDesc.Format = DXGI_FORMAT_R16_UINT;
            break;
        case DXGI_FORMAT_D16_UNORM:
            uavDesc.Format = DXGI_FORMAT_R16_UINT;
            break;
        default:
            break;
    }

    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = UINT_MAX;
    uavDesc.Texture2D.PlaneSlice = 0;
    pDevice->CreateUnorderedAccessView(pTexture, nullptr, &uavDesc, descriptorHandle);

    return true;
}
static const char *ShaderModelToString(D3D_SHADER_MODEL shader_model) {
    switch (shader_model) {
        case D3D_SHADER_MODEL_NONE: return "Not Support";
        case D3D_SHADER_MODEL_5_1: return "ShaderModel5_1";
        case D3D_SHADER_MODEL_6_0: return "ShaderModel6_0";
        case D3D_SHADER_MODEL_6_1: return "ShaderModel6_1";
        case D3D_SHADER_MODEL_6_2: return "ShaderModel6_2";
        case D3D_SHADER_MODEL_6_3: return "ShaderModel6_3";
        case D3D_SHADER_MODEL_6_4: return "ShaderModel6_4";
        case D3D_SHADER_MODEL_6_5: return "ShaderModel6_5";
        case D3D_SHADER_MODEL_6_6: return "ShaderModel6_6";
        case D3D_SHADER_MODEL_6_7: return "ShaderModel6_7";
        case D3D_SHADER_MODEL_6_8: return "ShaderModel6_8";
        case D3D_SHADER_MODEL_6_9: return "ShaderModel6_9";
        default: return "ShaderModel6_0";
    }
}

UNITY_INTERFACE_EXPORT bool UNITY_INTERFACE_API CheckBindlessSupport() {
    if (g_unityGraphics_D3D12 != nullptr) {
        ID3D12Device *device = g_unityGraphics_D3D12->GetDevice();
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {D3D_SHADER_MODEL_6_6};
        const HRESULT hr = device->CheckFeatureSupport(
            D3D12_FEATURE_SHADER_MODEL,
            &shaderModel,
            sizeof(shaderModel)
        );

        if (SUCCEEDED(hr) && shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6) {
            UNITY_LOG(g_Log, "Current GPU's Support ShaderModel6_6");
            return true;
        }

        const std::string result = std::format(
            "Current GPU's Highest Shader model is {}",
            ShaderModelToString(shaderModel.HighestShaderModel)
        );
        UNITY_LOG_ERROR(g_Log, result.c_str());
        return false;
    }

    UNITY_LOG_ERROR(g_Log, "D3D12 device Not ready");
    return false;
}
}
