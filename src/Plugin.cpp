#ifndef _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#define _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
#endif
#include "Plugin.h"
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3d12")

#include <atomic>
#include <d3d12.h>
#include <vector>
#include <mutex>

#include <dxgi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <IUnityInterface.h>
#include <IUnityGraphics.h>
#include <IUnityGraphicsD3D12.h>


extern "C" {
// Called by Unity to load the plugin and provide the interfaces pointer
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces *unityInterfaces) {
#if SUPPORT_VULKAN
    if (s_Graphics->GetRenderer() == kUnityGfxRendererNull) {
        extern void RenderAPI_Vulkan_OnPluginLoad(IUnityInterfaces *);
        RenderAPI_Vulkan_OnPluginLoad(unityInterfaces);
    }
#endif // SUPPORT_VULKAN
}

// Called by Unity when the plugin is unloaded
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API UnityPluginUnload() {
}
}
