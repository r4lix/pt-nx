#include "engine/render/render_util.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/core/log.h"
#include "engine/render/mesh.h"
#include "engine/render/pipeline_cache.h"

namespace pt {

bool g_checkpoints = false;
bool g_pipeline_stats = false;  // set by vk::Context when PT_SHADER_STATS is set and the driver has VK_KHR_pipeline_executable_properties

namespace {

// Test aid (PT_SHADER_STATS): logs what the driver reports for every pipeline it compiles (registers, instructions, warps per SM,
// spills), and the compiled assembly of the shaders named in the variable ("light_all.frag,gbuffer.frag", "*" for every one).
void ReportPipeline(VkDevice device, VkPipeline pipeline, const char* name) {
    if (!g_pipeline_stats || !pipeline || !vkGetPipelineExecutablePropertiesKHR || !vkGetPipelineExecutableStatisticsKHR) {
        return;
    }
    static const char* filter = std::getenv("PT_SHADER_STATS");
    const bool want_asm = filter && name && (std::strcmp(filter, "*") == 0 || std::strstr(filter, name) != nullptr);
    VkPipelineInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    info.pipeline = pipeline;
    uint32_t count = 0;
    vkGetPipelineExecutablePropertiesKHR(device, &info, &count, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> props(count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    vkGetPipelineExecutablePropertiesKHR(device, &info, &count, props.data());
    for (uint32_t i = 0; i < count; ++i) {
        VkPipelineExecutableInfoKHR exe{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        exe.pipeline = pipeline;
        exe.executableIndex = i;
        uint32_t stat_count = 0;
        vkGetPipelineExecutableStatisticsKHR(device, &exe, &stat_count, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> stats(stat_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        vkGetPipelineExecutableStatisticsKHR(device, &exe, &stat_count, stats.data());
        std::string line;
        for (const VkPipelineExecutableStatisticKHR& st : stats) {
            line += " | ";
            line += st.name;
            line += ' ';
            switch (st.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: line += st.value.b32 ? "yes" : "no"; break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: line += std::to_string(st.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: line += std::to_string(st.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: line += std::to_string(st.value.f64); break;
            default: break;
            }
        }
        LogInfo("shader stats: {} {}{}", name ? name : "?", props[i].name, line);
        if (!want_asm || !vkGetPipelineExecutableInternalRepresentationsKHR) {
            continue;
        }
        uint32_t ir_count = 0;
        vkGetPipelineExecutableInternalRepresentationsKHR(device, &exe, &ir_count, nullptr);
        std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(ir_count, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
        vkGetPipelineExecutableInternalRepresentationsKHR(device, &exe, &ir_count, irs.data());
        std::vector<std::vector<char>> data(ir_count);
        for (uint32_t r = 0; r < ir_count; ++r) {
            data[r].resize(irs[r].dataSize + 1, 0);
            irs[r].pData = data[r].data();
        }
        vkGetPipelineExecutableInternalRepresentationsKHR(device, &exe, &ir_count, irs.data());
        for (uint32_t r = 0; r < ir_count; ++r) {
            if (std::strstr(irs[r].name, "assembly") == nullptr) {
                continue;  // the NIR text is long and not needed
            }
            const char* text = data[r].data();
            int n = 0;
            while (*text) {
                const char* end = std::strchr(text, '\n');
                const size_t len = end ? static_cast<size_t>(end - text) : std::strlen(text);
                LogInfo("asm {} {} {:04}: {}", name, props[i].name, n++, std::string_view(text, len));
                text += len + (end ? 1 : 0);
            }
        }
    }
}

VkPipelineCreateFlags CaptureFlags() {
    return g_pipeline_stats ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0;
}

}

void UseTargets(VkCommandBuffer cmd, std::initializer_list<TargetUse> uses) {
    VkImageMemoryBarrier2 barriers[16];
    uint32_t count = 0;
    for (const TargetUse& use : uses) {
        if (!use.target || !use.target->Valid() || count == std::size(barriers)) {
            continue;
        }
        VkImageMemoryBarrier2& b = barriers[count++];
        b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = use.target->layout;
        b.newLayout = use.layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = use.target->image.image;
        b.subresourceRange = {use.target->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        use.target->layout = use.layout;
    }
    if (count == 0) {
        return;
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = count;
    dependency.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void BeginLabel(VkCommandBuffer cmd, const char* name) {
    if (g_checkpoints && vkCmdSetCheckpointNV) {
        vkCmdSetCheckpointNV(cmd, name);
    }
    if (vkCmdBeginDebugUtilsLabelEXT) {
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
    }
}

void EndLabel(VkCommandBuffer cmd) {
    if (vkCmdEndDebugUtilsLabelEXT) {
        vkCmdEndDebugUtilsLabelEXT(cmd);
    }
}

void SetViewport(VkCommandBuffer cmd, VkRect2D area) {
    VkViewport viewport{static_cast<float>(area.offset.x), static_cast<float>(area.offset.y), static_cast<float>(area.extent.width),
                        static_cast<float>(area.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &area);
}

void BeginPass(VkCommandBuffer cmd, VkRect2D area, std::span<const ColorOutput> colors, RenderTarget* depth, bool depth_read_only, bool clear_depth) {
    VkRenderingAttachmentInfo attachments[8];
    uint32_t count = 0;
    for (const ColorOutput& c : colors) {
        VkRenderingAttachmentInfo& a = attachments[count++];
        a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = c.target->image.view;
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = c.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.clearValue.color = c.clear_value;
    }
    VkRenderingAttachmentInfo depth_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
        depth_info.imageView = depth->image.view;
        depth_info.imageLayout = depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_info.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_info.storeOp = depth_read_only ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
        depth_info.clearValue.depthStencil = {0.0f, 0};
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = area;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = count;
    rendering.pColorAttachments = attachments;
    rendering.pDepthAttachment = depth ? &depth_info : nullptr;
    vkCmdBeginRendering(cmd, &rendering);
    SetViewport(cmd, area);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
}

void BeginPass(VkCommandBuffer cmd, VkExtent2D extent, std::initializer_list<ColorOutput> colors, RenderTarget* depth, bool depth_read_only,
               bool clear_depth) {
    BeginPass(cmd, VkRect2D{{0, 0}, extent}, std::span<const ColorOutput>(colors.begin(), colors.size()), depth, depth_read_only, clear_depth);
}

VkPipeline CreateGraphicsPipeline(VkDevice device, const PipelineDesc& desc) {
    VkShaderModule vert = vk::LoadShaderModule(device, desc.vertex);
    VkShaderModule frag = desc.fragment ? vk::LoadShaderModule(device, desc.fragment) : VK_NULL_HANDLE;
    if (!vert || (desc.fragment && !frag)) {
        if (vert) {
            vkDestroyShaderModule(device, vert, nullptr);
        }
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    const VkSpecializationMapEntry spec_entries[2] = {{0, 0, sizeof(uint32_t)}, {1, sizeof(uint32_t), sizeof(uint32_t)}};
    const uint32_t spec_data[2] = {desc.spec_flags, desc.spec_mode};
    VkSpecializationInfo spec_info{2, spec_entries, sizeof(spec_data), spec_data};
    if (desc.specialize) {
        stages[1].pSpecializationInfo = &spec_info;
    }
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv0)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv1)},
        {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        {6, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, joints)},
        {7, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, weights)},
        {8, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv2)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.mesh_input) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(std::size(attributes));
        vertex_input.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cull;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = desc.depth_bias ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = desc.depth_compare;
    std::vector<VkPipelineColorBlendAttachmentState> blends(desc.colors.size());
    for (size_t i = 0; i < blends.size(); ++i) {
        VkPipelineColorBlendAttachmentState& b = blends[i];
        b = {};
        b.colorWriteMask = i < desc.write_masks.size() ? desc.write_masks[i] : desc.write_mask;
        b.colorBlendOp = VK_BLEND_OP_ADD;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
        switch (i < desc.blends.size() ? desc.blends[i] : desc.blend) {
        case BlendMode::None: break;
        case BlendMode::Alpha:
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::PremultipliedFadeAlpha:
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case BlendMode::Additive:
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::ProbeLerp:
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BlendMode::ProbeAccumulate:
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            break;
        }
    }
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blends.size());
    blend.pAttachments = blends.data();
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                       VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = desc.depth_bias ? 5u : 4u;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<uint32_t>(desc.colors.size());
    rendering.pColorAttachmentFormats = desc.colors.data();
    rendering.depthAttachmentFormat = desc.depth;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = frag ? 2u : 1u;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = desc.layout;
    info.flags |= CaptureFlags();
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vk::Check(vkCreateGraphicsPipelines(device, vk::g_pipeline_cache, 1, &info, nullptr, &pipeline), desc.fragment ? desc.fragment : desc.vertex)) {
        pipeline = VK_NULL_HANDLE;
    }
    ReportPipeline(device, pipeline, desc.fragment ? desc.fragment : desc.vertex);
    vkDestroyShaderModule(device, vert, nullptr);
    if (frag) {
        vkDestroyShaderModule(device, frag, nullptr);
    }
    return pipeline;
}

VkPipeline CreateComputePipeline(VkDevice device, VkPipelineLayout layout, const char* shader) {
    VkShaderModule module = vk::LoadShaderModule(device, shader);
    if (!module) {
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    info.flags |= CaptureFlags();
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!vk::Check(vkCreateComputePipelines(device, vk::g_pipeline_cache, 1, &info, nullptr, &pipeline), shader)) {
        pipeline = VK_NULL_HANDLE;
    }
    ReportPipeline(device, pipeline, shader);
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

}
