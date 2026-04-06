#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <cstdint>

class VkPipelineBuilder {
public:
    VkPipelineBuilder(VkDevice device, VkPipelineCache cache = VK_NULL_HANDLE)
        : device_(device), cache_(cache) { reset(); }

    VkPipelineBuilder& setShaders(VkShaderModule vert, VkShaderModule frag)
    {
        shader_stages_[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages_[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stages_[0].module = vert;
        shader_stages_[0].pName = "main";

        shader_stages_[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages_[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shader_stages_[1].module = frag;
        shader_stages_[1].pName = "main";
        return *this;
    }

    VkPipelineBuilder& setVertexInput(const VkVertexInputBindingDescription* binding, uint32_t bindingCount,
                                       const VkVertexInputAttributeDescription* attributes, uint32_t attributeCount)
    {
        vertex_input_.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_.vertexBindingDescriptionCount = bindingCount;
        vertex_input_.pVertexBindingDescriptions = binding;
        vertex_input_.vertexAttributeDescriptionCount = attributeCount;
        vertex_input_.pVertexAttributeDescriptions = attributes;
        return *this;
    }

    VkPipelineBuilder& setTopology(VkPrimitiveTopology topology)
    {
        input_assembly_.topology = topology;
        return *this;
    }

    VkPipelineBuilder& setCullMode(VkCullModeFlags mode)
    {
        rasterizer_.cullMode = mode;
        return *this;
    }

    VkPipelineBuilder& setFrontFace(VkFrontFace face)
    {
        rasterizer_.frontFace = face;
        return *this;
    }

    VkPipelineBuilder& setDepthBias(float constantFactor, float slopeFactor)
    {
        rasterizer_.depthBiasEnable = VK_TRUE;
        rasterizer_.depthBiasConstantFactor = constantFactor;
        rasterizer_.depthBiasSlopeFactor = slopeFactor;
        return *this;
    }

    VkPipelineBuilder& setDepthTest(VkBool32 testEnable, VkBool32 writeEnable, VkCompareOp compareOp = VK_COMPARE_OP_LESS_OR_EQUAL)
    {
        depth_stencil_.depthTestEnable = testEnable;
        depth_stencil_.depthWriteEnable = writeEnable;
        depth_stencil_.depthCompareOp = compareOp;
        return *this;
    }

    VkPipelineBuilder& setColorBlend(const VkPipelineColorBlendAttachmentState* attachment, uint32_t count = 1)
    {
        color_blend_.attachmentCount = count;
        color_blend_.pAttachments = attachment;
        return *this;
    }

    VkPipelineBuilder& setNoColorAttachments()
    {
        color_blend_.attachmentCount = 0;
        color_blend_.pAttachments = nullptr;
        return *this;
    }

    VkPipelineBuilder& setRenderPass(VkRenderPass renderPass, uint32_t subpass = 0)
    {
        render_pass_ = renderPass;
        subpass_ = subpass;
        return *this;
    }

    VkPipelineBuilder& setLayout(VkPipelineLayout layout)
    {
        layout_ = layout;
        return *this;
    }

    VkResult build(VkPipeline* outPipeline)
    {
        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = shader_stages_;
        info.pVertexInputState = &vertex_input_;
        info.pInputAssemblyState = &input_assembly_;
        info.pViewportState = &viewport_state_;
        info.pRasterizationState = &rasterizer_;
        info.pMultisampleState = &multisampling_;
        info.pDepthStencilState = &depth_stencil_;
        info.pColorBlendState = &color_blend_;
        info.pDynamicState = &dynamic_state_;
        info.layout = layout_;
        info.renderPass = render_pass_;
        info.subpass = subpass_;

        return vkCreateGraphicsPipelines(device_, cache_, 1, &info, nullptr, outPipeline);
    }

private:
    void reset()
    {
        shader_stages_[0] = {};
        shader_stages_[1] = {};

        vertex_input_ = {};
        vertex_input_.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        input_assembly_ = {};
        input_assembly_.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        viewport_state_ = {};
        viewport_state_.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state_.viewportCount = 1;
        viewport_state_.scissorCount = 1;

        rasterizer_ = {};
        rasterizer_.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer_.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer_.lineWidth = 1.0f;
        rasterizer_.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer_.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        multisampling_ = {};
        multisampling_.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        depth_stencil_ = {};
        depth_stencil_.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_.depthTestEnable = VK_TRUE;
        depth_stencil_.depthWriteEnable = VK_TRUE;
        depth_stencil_.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        color_blend_ = {};
        color_blend_.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_.attachmentCount = 1;

        dynamic_states_ = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        dynamic_state_ = {};
        dynamic_state_.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_.dynamicStateCount = static_cast<uint32_t>(dynamic_states_.size());
        dynamic_state_.pDynamicStates = dynamic_states_.data();

        layout_ = VK_NULL_HANDLE;
        render_pass_ = VK_NULL_HANDLE;
        subpass_ = 0;
    }

    VkDevice device_;
    VkPipelineCache cache_;

    VkPipelineShaderStageCreateInfo shader_stages_[2];
    VkPipelineVertexInputStateCreateInfo vertex_input_;
    VkPipelineInputAssemblyStateCreateInfo input_assembly_;
    VkPipelineViewportStateCreateInfo viewport_state_;
    VkPipelineRasterizationStateCreateInfo rasterizer_;
    VkPipelineMultisampleStateCreateInfo multisampling_;
    VkPipelineDepthStencilStateCreateInfo depth_stencil_;
    VkPipelineColorBlendStateCreateInfo color_blend_;
    std::vector<VkDynamicState> dynamic_states_;
    VkPipelineDynamicStateCreateInfo dynamic_state_;

    VkPipelineLayout layout_;
    VkRenderPass render_pass_;
    uint32_t subpass_;
};
