#include <mutex>
#include <wrl/client.h>
#ifndef _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#endif
#pragma once

#include <cstdint>
#include <d3d12.h>
#include <IUnityInterface.h>
#include <IUnityRenderingExtensions.h>


using Microsoft::WRL::ComPtr;



extern "C" {
// Initializes D3D12 bindless UAV heap. Returns capacity on success, 0 on failure.
UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API D3D12_InitBindlessUAVHeap(uint32_t capacity);

// Allocates a descriptor slot and writes a UAV for the given resource at that slot.
// Returns slot index, or 0xFFFFFFFF on failure.
UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API D3D12_SetUAVTextureAt(uint32_t slotIndex /* 0xFFFFFFFF to auto-alloc */, void *nativeResource,
                                                                          DXGI_FORMAT format, uint32_t mipSlice);

// Releases a previously allocated slot.
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API D3D12_ReleaseSlot(uint32_t slotIndex);

// Returns GPU descriptor heap start pointer (for shader indexing), as uint64.
UNITY_INTERFACE_EXPORT uint64_t UNITY_INTERFACE_API D3D12_GetUAVHeapGpuStart();

// Returns the descriptor size for D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV.
UNITY_INTERFACE_EXPORT uint32_t UNITY_INTERFACE_API D3D12_GetDescriptorSizeCSU();

// Returns a render event function pointer. When invoked from Unity with userData==1,
// it binds the plugin's UAV heap to the current command list for compute/graphics.
UNITY_INTERFACE_EXPORT UnityRenderingEventAndData UNITY_INTERFACE_API D3D12_GetBindlessRenderEvent();
}
