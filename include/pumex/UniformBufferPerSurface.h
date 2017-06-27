//
// Copyright(c) 2017 Pawe� Ksi�opolski ( pumexx )
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#pragma once
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <pumex/Export.h>
#include <pumex/DeviceMemoryAllocator.h>
#include <pumex/Device.h>
#include <pumex/Command.h>
#include <pumex/Pipeline.h>
#include <pumex/utils/Buffer.h>
#include <pumex/utils/Log.h>


// Uniform buffer that stores different C++ struct per surface

namespace pumex
{

template <typename T>
class UniformBufferPerSurface : public DescriptorSetSource
{
public:
  UniformBufferPerSurface()                                          = delete;
  explicit UniformBufferPerSurface(std::weak_ptr<DeviceMemoryAllocator> allocator, uint32_t activeCount = 1, VkBufferUsageFlagBits additionalFlags = (VkBufferUsageFlagBits)0);
  UniformBufferPerSurface(const UniformBufferPerSurface&)            = delete;
  UniformBufferPerSurface& operator=(const UniformBufferPerSurface&) = delete;
  ~UniformBufferPerSurface();

  inline void set( Surface* surface, const T& data );
  inline T    get( Surface* surface ) const;
  void        getDescriptorSetValues(VkSurfaceKHR surface, uint32_t index, std::vector<DescriptorSetValue>& values) const override;
  void        setDirty();
  void        validate(Surface* surface);

  inline void setActiveIndex(uint32_t index);
  inline uint32_t getActiveIndex() const;

private:
  struct PerSurfaceData
  {
    PerSurfaceData(uint32_t ac, VkDevice d)
      : device{ d }
    {
      dirty.resize(ac, true);
      uboBuffer.resize(ac, VK_NULL_HANDLE);
      memoryBlock.resize(ac, DeviceMemoryBlock());
    }
    void setDirty()
    {
      for (auto& d : dirty)
        d = true;
    }

    T                               uboData;
    VkDevice                        device;
    std::vector<bool>               dirty;
    std::vector<VkBuffer>           uboBuffer;
    std::vector<DeviceMemoryBlock>  memoryBlock;
  };
  std::unordered_map<VkSurfaceKHR, PerSurfaceData> perSurfaceData;
  std::weak_ptr<DeviceMemoryAllocator>             allocator;
  VkBufferUsageFlagBits                            additionalFlags;
  uint32_t                                         activeCount;
  uint32_t                                         activeIndex = 0;

};

template <typename T>
UniformBufferPerSurface<T>::UniformBufferPerSurface(std::weak_ptr<DeviceMemoryAllocator> a, uint32_t ac, VkBufferUsageFlagBits af)
  : allocator{ a }, additionalFlags{ af }, activeCount{ ac }
{
}

template <typename T>
UniformBufferPerSurface<T>::~UniformBufferPerSurface()
{
  std::shared_ptr<DeviceMemoryAllocator> alloc = allocator.lock();
  for (auto& pdd : perSurfaceData)
  {
    for (uint32_t i = 0; i < activeCount; ++i)
    {
      vkDestroyBuffer(pdd.second.device, pdd.second.uboBuffer[i], nullptr);
      alloc->deallocate(pdd.second.device, pdd.second.memoryBlock[i]);
    }
  }
}


template <typename T>
void UniformBufferPerSurface<T>::set(Surface* surface, const T& data)
{
  auto it = perSurfaceData.find(surface->surface);
  if (it == perSurfaceData.end())
    it = perSurfaceData.insert({ surface->surface, PerSurfaceData(activeCount, surface->device.lock()->device) }).first;
  it->second.uboData = data;
  it->second.setDirty();
}

template <typename T>
T UniformBufferPerSurface<T>::get(Surface* surface) const
{
  auto it = perSurfaceData.find(surface->surface);
  if (it == perSurfaceData.end())
    it = perSurfaceData.insert({ surface->surface, PerSurfaceData(activeCount, surface->device.lock()->device) }).first;
  return it->second.uboData;
}

template <typename T>
void UniformBufferPerSurface<T>::getDescriptorSetValues(VkSurfaceKHR surface, uint32_t index, std::vector<DescriptorSetValue>& values) const
{
  auto pddit = perSurfaceData.find(surface);
  CHECK_LOG_THROW(pddit == perSurfaceData.end(), "UniformBufferPerSurface<T>::getDescriptorBufferInfo : uniform buffer was not validated");

  values.push_back( DescriptorSetValue(pddit->second.uboBuffer[index % activeCount], 0, sizeof(T)));
}

template <typename T>
void UniformBufferPerSurface<T>::setDirty()
{
  for (auto& pdd : perDeviceData)
    pdd.second->setDirty();
}

template <typename T>
void UniformBufferPerSurface<T>::validate(Surface* surface)
{
  Device* devicePtr = surface->device.lock().get();
  VkDevice device   = devicePtr->device;
  auto it           = perSurfaceData.find(surface->surface);
  if (it == perSurfaceData.end())
    it = perSurfaceData.insert({ surface->surface, PerSurfaceData(activeCount, device) }).first;
  if (!it->second.dirty[activeIndex])
    return;

  bool memoryIsLocal = ((allocator.lock()->getMemoryPropertyFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (it->second.uboBuffer[activeIndex] == VK_NULL_HANDLE)
  {
    std::shared_ptr<DeviceMemoryAllocator> alloc = allocator.lock();
    VkBufferCreateInfo bufferCreateInfo{};
      bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bufferCreateInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | additionalFlags | (memoryIsLocal ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0);
      bufferCreateInfo.size  = std::max<VkDeviceSize>(1, sizeof(T));
    VK_CHECK_LOG_THROW(vkCreateBuffer(device, &bufferCreateInfo, nullptr, &it->second.uboBuffer[activeIndex]), "Cannot create buffer");
    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, it->second.uboBuffer[activeIndex], &memReqs);
    it->second.memoryBlock[activeIndex] = alloc->allocate(devicePtr, memReqs);
    CHECK_LOG_THROW(it->second.memoryBlock[activeIndex].alignedSize == 0, "Cannot create UBO");
    VK_CHECK_LOG_THROW(vkBindBufferMemory(device, it->second.uboBuffer[activeIndex], it->second.memoryBlock[activeIndex].memory, it->second.memoryBlock[activeIndex].alignedOffset), "Cannot bind memory to buffer");

    notifyDescriptorSets();
  }
  if (memoryIsLocal)
  {
    std::shared_ptr<StagingBuffer> stagingBuffer = devicePtr->acquireStagingBuffer(&it->second.uboData, sizeof(T));
    auto staggingCommandBuffer = devicePtr->beginSingleTimeCommands(surface->commandPool.get());
    VkBufferCopy copyRegion{};
    copyRegion.size = sizeof(T);
    staggingCommandBuffer->cmdCopyBuffer(stagingBuffer->buffer, it->second.uboBuffer[activeIndex], copyRegion);
    devicePtr->endSingleTimeCommands(staggingCommandBuffer, surface->presentationQueue);
    devicePtr->releaseStagingBuffer(stagingBuffer);
  }
  else
  {
    uint8_t *pData;
    VK_CHECK_LOG_THROW(vkMapMemory(device, it->second.memoryBlock[activeIndex].memory, it->second.memoryBlock[activeIndex].alignedOffset, sizeof(T), 0, (void **)&pData), "Cannot map memory");
    memcpy(pData, &it->second.uboData, sizeof(T));
    vkUnmapMemory(device, it->second.memoryBlock[activeIndex].memory);
  }
  it->second.dirty[activeIndex] = false;
}

template <typename T>
void UniformBufferPerSurface<T>::setActiveIndex(uint32_t index) { activeIndex = index % activeCount; }
template <typename T>
uint32_t UniformBufferPerSurface<T>::getActiveIndex() const { return activeIndex; }


}