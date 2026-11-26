// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/gow3_buffer_descriptor_template.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

void Pipeline::InitializeBufferDescriptorTemplate(u32 buffer_count) {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_DESCRIPTOR_TEMPLATE");
        return env && env[0] == '1';
    }();
    if (!enabled || !uses_push_descriptors || buffer_count < 4 ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorUpdateTemplate ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDescriptorSetWithTemplateKHR) {
        return;
    }
    std::vector<vk::DescriptorUpdateTemplateEntry> entries;
    entries.reserve(buffer_count);
    for (u32 i = 0; i < buffer_count; ++i) {
        entries.push_back(vk::DescriptorUpdateTemplateEntry{
            .dstBinding = i,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .offset = i * sizeof(vk::DescriptorBufferInfo),
            .stride = sizeof(vk::DescriptorBufferInfo),
        });
    }
    const vk::DescriptorUpdateTemplateCreateInfo info{
        .descriptorUpdateEntryCount = buffer_count,
        .pDescriptorUpdateEntries = entries.data(),
        .templateType = vk::DescriptorUpdateTemplateType::ePushDescriptorsKHR,
        .pipelineBindPoint = vk::PipelineBindPoint::eCompute,
        .pipelineLayout = *pipeline_layout,
        .set = 0,
    };
    auto [result, update] = instance.GetDevice().createDescriptorUpdateTemplateUnique(info);
    if (result == vk::Result::eSuccess) {
        buffer_descriptor_template = std::move(update);
        buffer_descriptor_count = buffer_count;
    }
}

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data,
                             std::span<const vk::DescriptorBufferInfo> source_buffers,
                             std::span<const vk::DescriptorImageInfo> source_images) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (buffer_descriptor_template && source_images.empty() &&
            Detail::MatchesBufferDescriptorTemplate(
                std::span<const vk::WriteDescriptorSet>{set_writes}, source_buffers,
                buffer_descriptor_count, vk::DescriptorType::eStorageBuffer)) {
            scheduler.ReserveRecordData(source_buffers.size_bytes() +
                                        alignof(vk::DescriptorBufferInfo) + 64);
            const auto buffers = scheduler.RecordData(source_buffers);
            const vk::DescriptorUpdateTemplate update = *buffer_descriptor_template;
            scheduler.Record([update, layout, buffers](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetWithTemplateKHR(update, layout, 0,
                                                        static_cast<const void*>(buffers.data()));
            });
            return;
        }
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
