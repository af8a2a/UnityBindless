//
// Created by 11252 on 2025/11/5.
//

#ifndef UNITYPLUGIN_BINDLESSMANAGER_HPP
#define UNITYPLUGIN_BINDLESSMANAGER_HPP


#pragma once

#include <cstdint>
#include <d3d12.h>
#include <mutex>
#include <wrl/client.h>
#include <MinHook.h>


using Microsoft::WRL::ComPtr;


class BindlessManager {
public:
    static BindlessManager &Instance();

    bool Initialize(ID3D12Device *device, uint32_t descriptorCount = 65536);

    int AllocDescriptor(ID3D12Resource *pResource, uint32_t index = UINT32_MAX);

    int GetDescriptorCount() const { return m_DescriptorCount; }

    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUBaseHandle() const { return m_GPUHeapBase; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUBaseHandle() const { return m_CPUHeapBase; }
    ID3D12DescriptorHeap *GetHeap() const { return m_Heap.Get(); }

private:
    BindlessManager() = default;

    std::mutex m_Mutex;

    ComPtr<ID3D12DescriptorHeap> m_Heap;
    ComPtr<ID3D12Device> m_Device;
    D3D12_CPU_DESCRIPTOR_HANDLE m_CPUHeapBase{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_GPUHeapBase{};
    UINT m_DescriptorSize = 0;
    UINT m_DescriptorCount = 0;
    UINT m_NextAllocIndex = 0;
};

#endif //UNITYPLUGIN_BINDLESSMANAGER_HPP
