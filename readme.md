# UnityBindless

`UnityBindless` is a Windows native Unity plugin for enabling D3D12 bindless resource access. It is based on the DELTation bindless plugin work and focuses on Shader Model 6.6 style direct indexing for SRV/UAV resources.

## What it does

The plugin hooks Unity's D3D12 path to:

- patch root signatures with direct-indexing flags;
- reserve and track a plugin-owned descriptor range inside Unity's shader-visible CBV/SRV/UAV heap;
- expose native exports for writing SRV and UAV descriptors into that range;
- report whether the active GPU supports Shader Model 6.6 or newer.

The current implementation is D3D12-only and intended for Windows builds.

## Repository layout

- `src/Plugin.cpp` - plugin entry points, hook installation, descriptor heap tracking, and exported API implementation
- `src/Plugin.h` - exported native functions
- `src/HookWrapper.hpp` - small MinHook wrapper used by the plugin
- `PluginAPI/` - Unity native plugin headers vendored into the repository
- `External/minhook/` - MinHook submodule used for API detouring

## Requirements

- Windows with a D3D12-capable GPU
- Unity project running the D3D12 graphics backend
- GPU/driver support for Shader Model 6.6+ if you want bindless indexing
- Visual Studio with C++ build tools and CMake 3.20+

## Build

Initialize submodules first:

```powershell
git submodule update --init --recursive
```

Configure and build:

```powershell
cmake -S . -B build
cmake --build build --config RelWithDebInfo
```

The DLL is written to `build/bin/RelWithDebInfo/UnityBindless.dll`.

If your local integration depends on prebuilt NRD or NRI libraries, pass the expected cache variables during configure, for example:

```powershell
cmake -S . -B build -DNRD_LIB_DEBUG=... -DNRD_LIB_RELEASE=... -DNRI_LIB_DEBUG=... -DNRI_LIB_RELEASE=...
```

## Unity integration

1. Copy `UnityBindless.dll` into your Unity project, typically under `Assets/Plugins/x86_64/`.
2. Run the project with the D3D12 backend enabled.
3. Let Unity load the plugin and initialize the graphics device.
4. Call the exported functions from your C# or native interop layer once the device is ready.

Useful exports:

- `WarmupPlugin()` - simple load check
- `CheckBindlessSupport()` - returns whether Shader Model 6.6 is available
- `GetSRVDescriptorHeapCount()` - returns the tracked shader-visible heap size
- `GetBindlessDescriptorStartIndex()` / `GetBindlessDescriptorCount()` - returns the reserved plugin-owned range
- `CreateSRVDescriptor(...)` / `CreateUAVDescriptor(...)` - writes descriptors into a chosen bindless slot

## Runtime behavior

The plugin installs hooks for `D3D12SerializeRootSignature`, `CreateDescriptorHeap`, and `SetDescriptorHeaps`. When Unity creates a large enough shader-visible CBV/SRV/UAV heap, the plugin extends it and reserves a bindless region at the end. At runtime it tracks the active heap and uses that cached range when creating descriptors.

## Troubleshooting

- `CheckBindlessSupport()` fails: verify the Unity player is running on D3D12 and the GPU supports Shader Model 6.6+.
- Descriptor creation returns `false`: make sure the plugin has observed a valid shader-visible heap and that the requested index falls inside the reported bindless range.
- Heap count is `0`: the plugin may not have seen Unity create or bind the target descriptor heap yet.

The plugin logs detailed diagnostics through Unity's native logging interface, so check the Unity console when debugging initialization or hook failures.
