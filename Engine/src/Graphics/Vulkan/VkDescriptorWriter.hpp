#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

class VkDescriptorWriter {
public:
    VkDescriptorWriter(VkDescriptorSet set) : set_(set) {}

    VkDescriptorWriter& writeBuffer(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo* bufferInfo)
    {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pBufferInfo = bufferInfo;
        writes_.push_back(write);
        return *this;
    }

    VkDescriptorWriter& writeImage(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* imageInfo)
    {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_;
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pImageInfo = imageInfo;
        writes_.push_back(write);
        return *this;
    }

    void update(VkDevice device)
    {
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes_.size()), writes_.data(), 0, nullptr);
    }

private:
    VkDescriptorSet set_;
    std::vector<VkWriteDescriptorSet> writes_;
};
