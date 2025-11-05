//
// Created by 11252 on 2025/11/5.
//

#include "BindlessManager.hpp"

#include <cassert>

BindlessManager& BindlessManager::Instance()
{
    static BindlessManager instance;
    return instance;
}


bool BindlessManager::Initialize(ID3D12Device* device, uint32_t descriptorCount)
{
    if (m_Heap) return true; // already initialized

    m_Device = device;
    m_DescriptorCount = descriptorCount;
    m_DescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = descriptorCount;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_Heap));
    if (FAILED(hr)) return false;

    m_CPUHeapBase = m_Heap->GetCPUDescriptorHandleForHeapStart();
    m_GPUHeapBase = m_Heap->GetGPUDescriptorHandleForHeapStart();

    return true;
}


int BindlessManager::AllocDescriptor(ID3D12Resource* pResource, uint32_t index)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Heap) return -1;

    if (index == UINT32_MAX)
        index = m_NextAllocIndex++;

    assert(index < m_DescriptorCount);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    auto desc = pResource->GetDesc();

    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Format = desc.Format;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
    }
    else
    {
        // support others as needed
        return -1;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_CPUHeapBase;
    handle.ptr += index * m_DescriptorSize;
    m_Device->CreateShaderResourceView(pResource, &srvDesc, handle);

    return static_cast<int>(index);
}
