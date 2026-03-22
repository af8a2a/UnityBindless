# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# First-time setup
git submodule update --init --recursive

# Configure
cmake -S . -B build

# Build
cmake --build build --config RelWithDebInfo   # output: build/bin/RelWithDebInfo/UnityBindless.dll
cmake --build build --config Debug            # output: build/bin/Debug/UnityBindless.dll
```

No automated test target exists. Manual validation: load the DLL in a Unity project on the D3D12 backend and smoke-test `WarmupPlugin()`, `CheckBindlessSupport()`, and descriptor creation paths.

## Architecture

This is a Windows-only D3D12 native plugin that enables bindless resource access (Shader Model 6.6+) in Unity by hooking D3D12 API functions at runtime using MinHook.

**Hook strategy:** `Plugin.cpp` installs detours on:
- `D3D12SerializeRootSignature` — patches every root signature to add `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `SAMPLER_HEAP_DIRECTLY_INDEXED` flags
- `ID3D12Device::CreateDescriptorHeap` (hooks all 15 device interface versions, Device–Device14) — intercepts shader-visible CBV/SRV/UAV heap creation to expand it by `kBindlessDescriptorReserveCount` (16 384) descriptors and record the heap pointer
- `ID3D12GraphicsCommandList::SetDescriptorHeaps` (up to 64 hooks for different command list types) — tracks which heap is currently bound

**Template detour slots (`HookWrapper.hpp`):** Because multiple device/command-list versions share the same function signature, each hook needs a unique static trampoline. `HookWrapper.hpp` provides a type-safe wrapper that stores the original function pointer and wires up MinHook.

**Public C API (`Plugin.h`):**
- `WarmupPlugin()` — initialization check
- `CheckBindlessSupport()` — validates SM 6.6+ GPU support
- `GetSRVDescriptorHeapCount()` — total descriptor count in the captured heap
- `GetBindlessDescriptorStartIndex()` — start of the reserved bindless range
- `GetBindlessDescriptorCount()` — size of the reserved bindless range
- `CreateSRVDescriptor(ID3D12Resource*, uint32_t index)` — write an SRV into the bindless range
- `CreateUAVDescriptor(ID3D12Resource*, uint32_t index)` — write a UAV into the bindless range

**Key constants (CMakeLists.txt compile definitions):**
- `MAIN_DESCRIPTOR_HEAP_MIN_COUNT` = 262 144
- `BINDLESS_DESCRIPTOR_RESERVE_COUNT` = 16 384 (4096 × 4)
- `MAX_DESCRIPTOR_COUNT` = 1 000 000

**Vendored/external — do not modify:**
- `PluginAPI/` — Unity native plugin headers
- `External/minhook/` — MinHook submodule

## Coding Style

C++20, MSVC `/W4` clean, 4-space indent, opening braces on the same line. Naming: exported functions `PascalCase`, constants `kPrefix`, globals `g_`, file-statics `s_`, members `m_`.

## Commit Style

Short imperative subjects with optional prefix: `fix:`, `update:`, `support`, e.g. `fix: guard descriptor heap capture`.
