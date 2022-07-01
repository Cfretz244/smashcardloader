// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include "VideoCommon/BoundingBox.h"

namespace Vulkan
{
class StagingBuffer;

class VKBoundingBox final : public BoundingBox
{
public:
  ~VKBoundingBox() override;

  bool Initialize() override;

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, const std::vector<BBoxType>& values) override;

private:
  bool CreateGPUBuffer();
  bool CreateReadbackBuffer();

  VkBuffer m_gpu_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_gpu_memory = VK_NULL_HANDLE;

  static constexpr size_t BUFFER_SIZE = sizeof(BBoxType) * NUM_BBOX_VALUES;

  std::unique_ptr<StagingBuffer> m_readback_buffer;
};

}  // namespace Vulkan
