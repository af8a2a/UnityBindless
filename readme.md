a fork of https://github.com/Delt06/aaaa-rp/tree/master/DELTationBindlessPlugin


# Project Overview

UnityBindless is a Unity native plugin that enables bindless resource access in DirectX 12 for Unity applications.

## Purpose

This plugin hooks into Unity's D3D12 rendering pipeline to enable bindless textures (Shader Model 6.6+ feature), allowing direct indexing of shader resources without traditional descriptor table bindings.

## Architecture

Core Components:

1. Plugin.cpp (src/Plugin.cpp:1) - Main plugin implementation
   - Hooks D3D12 API functions using MinHook library
   - Intercepts D3D12SerializeRootSignature to add bindless flags
   - Hooks CreateDescriptorHeap to capture CBV/SRV/UAV descriptor heap
   - Provides APIs for creating SRV/UAV descriptors at specific indices
2. HookWrapper.hpp (src/HookWrapper.hpp:1) - Templated MinHook wrapper
   - Simplifies function hooking/unhooking
   - Type-safe original function pointer storage
3. Plugin.h (src/Plugin.h:1) - Public API exports
   - GetSRVDescriptorHeapCount() - Returns descriptor heap size
   - CreateSRVDescriptor() - Creates shader resource view at index
   - CreateUAVDescriptor() - Creates unordered access view at index
   - CheckBindlessSupport() - Validates SM 6.6+ support

Dependencies:
- MinHook (External/minhook) - Function hooking library
- Unity Plugin API (PluginAPI/) - Unity native plugin interfaces

Build System:
- CMake-based (CMakeLists.txt:1)
- C++20 standard
- Outputs: UnityBindless.dll

## Key Features

1. Automatic Root Signature Patching - Adds bindless flags to all root signatures
2. Descriptor Management - Tracks Unity's descriptor heap for bindless access
3. Format Conversion - Handles depth-stencil format conversions for SRV/UAV
4. Hardware Validation - Checks for Shader Model 6.6+ support

Use Case

Enables Unity developers to use modern D3D12 bindless resource features for improved rendering performance, particularly useful for advanced rendering techniques requiring dynamic texture indexing.