#include "nnsdk/nvn/lsfg/motion.h"

#include <algorithm>
#include <cstdio>

#include "nnsdk/nvn/nvn.h"
#include "nnsdk/nvn/shaders/spirv.h"

namespace nvn::lsfg {

namespace {

#if defined(HAVE_fg_luma_SPV) && defined(HAVE_fg_down_SPV) && defined(HAVE_fg_patch_SPV) && defined(HAVE_fg_densify_SPV) && \
    defined(HAVE_fg_warp_SPV)
#define FG_SHADERS 1
#else
#define FG_SHADERS 0
#endif

// The push constants every pass has (shaders/fg_*.comp): its output's size, its source's size, 1 / the picture's size,
// a flag and one more value (patch: the coarser flow's size, packed)
struct Push {
    int32_t size[2], src_size[2];
    float inv[2];
    int32_t flags, extra;
    float t;  // fg_warp: where the picture goes between A (0) and B (1)
};

constexpr VkFormat kLumaFormat = VK_FORMAT_R32_SFLOAT, kFieldFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
// The dense flows: half floats (a storage format every driver has, and filterable), so the warp reads each with one
// bilinear tap instead of four loads
constexpr VkFormat kFlowFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The finest flow level by quality (the Quality row, the same switches as LSFG's flow): a quarter of the picture
// (Light), a third (Balanced), half (Sharp). Each step finer is about twice the work and follows smaller things.
uint32_t finest_for(float flow) { return flow >= 0.99f ? 2 : flow >= 0.49f ? 3 : 4; }
constexpr VkFormat kOut = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags kWork = VK_IMAGE_USAGE_STORAGE_BIT;

VkFormat unorm(VkFormat f) {  // (an sRGB picture is read and written as its stored values, as LSFG's chain does)
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}

uint32_t memory_type(uint32_t bits) {
    const VkPhysicalDeviceMemoryProperties& m = vk()->memory;
    for (uint32_t k = 0; k < m.memoryTypeCount; k++)
        if ((bits & (1u << k)) && (m.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return k;
    return UINT32_MAX;
}

VkExtent2D div(VkExtent2D e, uint32_t d) { return {std::max(1u, (e.width + d - 1) / d), std::max(1u, (e.height + d - 1) / d)}; }

void compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

}  // namespace

bool Motion::available() { return FG_SHADERS != 0; }

void Motion::add(Image& image, VkExtent2D extent, VkFormat format, VkFormat view_format, VkImageUsageFlags usage,
                 VkImageCreateFlags flags) {
    image = Image{};
    image.extent = {std::max(1u, extent.width), std::max(1u, extent.height)};
    image.format = format;
    image.view_format = view_format;
    image.usage = usage;
    image.flags = flags;
    all_.push_back(&image);
}

bool Motion::create(VkExtent2D extent, VkFormat format, float flow, std::string* why) {
    destroy();
    if (!available()) {
        *why = "this build has no shaders for it (tools/shaders.sh)";
        return false;
    }
    extent_ = extent;
    format_ = format;
    flow_ = flow;
    finest_ = finest_for(flow);
    if (!build_pipelines(why) || !build_images(why) || !build_sets(why)) {
        destroy();
        return false;
    }
    return true;
}

void Motion::destroy() {
    VkContext* c = vk();
    if (!c) return;
    VkDevice d = c->device;
    for (Image* i : all_) {
        if (i->view) vkDestroyImageView(d, i->view, nullptr);
        if (i->image) vkDestroyImage(d, i->image, nullptr);
        *i = Image{};
    }
    all_.clear();
    if (memory_) vkFreeMemory(d, memory_, nullptr);
    memory_ = VK_NULL_HANDLE;
    bytes_ = 0;
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    sampler_ = VK_NULL_HANDLE;
    for (Pipeline& p : passes_) {
        if (p.pipeline) vkDestroyPipeline(d, p.pipeline, nullptr);
        if (p.layout) vkDestroyPipelineLayout(d, p.layout, nullptr);
        if (p.set_layout) vkDestroyDescriptorSetLayout(d, p.set_layout, nullptr);
        p = Pipeline{};
    }
    initialized_ = false;
}

bool Motion::build_pipelines(std::string* why) {
#if FG_SHADERS
    VkContext* c = vk();
    constexpr VkDescriptorType S = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, T = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    struct Def {
        const uint32_t* code;
        size_t bytes;
        std::vector<VkDescriptorType> types;
    };
    const Def defs[kPasses] = {
        {k_fg_luma_spv, sizeof k_fg_luma_spv, {T, S}},
        {k_fg_down_spv, sizeof k_fg_down_spv, {S, S}},
        {k_fg_patch_spv, sizeof k_fg_patch_spv, {S, S, S, S}},
        {k_fg_densify_spv, sizeof k_fg_densify_spv, {S, S, S, S}},
        {k_fg_warp_spv, sizeof k_fg_warp_spv, {T, T, T, T, S}},
    };
    for (int k = 0; k < kPasses; k++) {
        Pipeline& p = passes_[k];
        std::vector<VkDescriptorSetLayoutBinding> b(defs[k].types.size());
        for (uint32_t i = 0; i < b.size(); i++) b[i] = {i, defs[k].types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = static_cast<uint32_t>(b.size());
        li.pBindings = b.data();
        if (vkCreateDescriptorSetLayout(c->device, &li, nullptr, &p.set_layout) != VK_SUCCESS)
            return *why = "vkCreateDescriptorSetLayout", false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &p.set_layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(c->device, &pl, nullptr, &p.layout) != VK_SUCCESS)
            return *why = "vkCreatePipelineLayout", false;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = defs[k].bytes;
        mi.pCode = defs[k].code;
        VkShaderModule module = VK_NULL_HANDLE;
        if (VkResult r = vkCreateShaderModule(c->device, &mi, nullptr, &module); r != VK_SUCCESS)
            return *why = std::string("vkCreateShaderModule: ") + vk_result_name(r), false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = p.layout;
        const VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline);
        vkDestroyShaderModule(c->device, module, nullptr);
        if (r != VK_SUCCESS) return *why = std::string("vkCreateComputePipelines: ") + vk_result_name(r), false;
    }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(c->device, &si, nullptr, &sampler_) != VK_SUCCESS) return *why = "vkCreateSampler", false;
    return true;
#else
    *why = "no shaders";
    return false;
#endif
}

bool Motion::build_images(std::string* why) {
    VkContext* c = vk();
    const VkFormat view = unorm(format_);
    const bool srgb = view != format_;
    // The pictures: as the window's (copied or blitted in), read through a UNORM view; the output too, written through one
    for (Image& f : frames_)
        add(f, extent_, format_, view, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    // The levels: a quarter of the picture, then halves, while both sides keep at least 16 texels (two patches)
    levels_ = 0;
    for (VkExtent2D e = div(extent_, finest_); levels_ < kLevels && e.width >= 16 && e.height >= 16; e = div(e, 2)) {
        for (int i = 0; i < 2; i++) add(lum_[i][levels_], e, kLumaFormat, kLumaFormat, kWork);
        for (int w = 0; w < 2; w++) {
            add(patches_[w][levels_], grid(e), kFieldFormat, kFieldFormat, kWork);
            add(flows_[w][levels_], e, kFlowFormat, kFlowFormat, kWork | VK_IMAGE_USAGE_SAMPLED_BIT);
        }
        levels_++;
    }
    if (!levels_) return *why = "the picture is too small", false;
    // (the window's own format with a UNORM view for the shader when it's an sRGB one: the blit to the swapchain then
    // converts the made picture as it does the real one)
    add(output_, extent_, srgb ? VK_FORMAT_R8G8B8A8_SRGB : kOut, kOut,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0);

    auto has = [&](VkFormat f, VkFormatFeatureFlags need) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->gpu, f, &fp);
        return (fp.optimalTilingFeatures & need) == need;
    };
    char buf[160];
    for (Image* i : all_) {
        VkFormatFeatureFlags need = 0;
        if (i->usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (i->usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (!has(i->view_format, need)) {
            snprintf(buf, sizeof buf, "format %d can't be used for it", i->view_format);
            return *why = buf, false;
        }
    }
    uint32_t bits = ~0u;
    for (Image* i : all_) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.flags = i->flags;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = i->format;
        ii.extent = {i->extent.width, i->extent.height, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = i->usage;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (VkResult r = vkCreateImage(c->device, &ii, nullptr, &i->image); r != VK_SUCCESS) {
            snprintf(buf, sizeof buf, "vkCreateImage %ux%u format %d: %s", i->extent.width, i->extent.height, i->format,
                     vk_result_name(r));
            return *why = buf, false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(c->device, i->image, &req);
        bits &= req.memoryTypeBits;
        i->offset = (bytes_ + req.alignment - 1) / req.alignment * req.alignment;
        bytes_ = i->offset + req.size;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};  // (one allocation for all, as LSFG's chain)
    ai.allocationSize = bytes_;
    ai.memoryTypeIndex = memory_type(bits);
    if (ai.memoryTypeIndex == UINT32_MAX) return *why = "no memory type for all the images", false;
    if (VkResult r = vkAllocateMemory(c->device, &ai, nullptr, &memory_); r != VK_SUCCESS)
        return *why = std::string("vkAllocateMemory: ") + vk_result_name(r), false;
    for (Image* i : all_) {
        if (VkResult r = vkBindImageMemory(c->device, i->image, memory_, i->offset); r != VK_SUCCESS)
            return *why = std::string("vkBindImageMemory: ") + vk_result_name(r), false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = i->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (i->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) vi.pNext = &usage;  // (storage only through the UNORM view)
        vi.image = i->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = i->view_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (VkResult r = vkCreateImageView(c->device, &vi, nullptr, &i->view); r != VK_SUCCESS)
            return *why = std::string("vkCreateImageView: ") + vk_result_name(r), false;
    }
    return true;
}

bool Motion::build_sets(std::string* why) {
    VkContext* c = vk();
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets * 4},  // (at most 4 in a set)
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 4}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = kSets;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(c->device, &pi, nullptr, &pool_) != VK_SUCCESS) return *why = "vkCreateDescriptorPool", false;
    auto make = [&](Pass pass, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &passes_[pass].set_layout;
        return vkAllocateDescriptorSets(c->device, &ai, out) == VK_SUCCESS;
    };
    // Writes a set: each image in binding order, sampled (with the linear sampler) or storage
    std::vector<VkDescriptorImageInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](VkDescriptorSet set, std::initializer_list<std::pair<const Image*, bool>> images) {
        uint32_t binding = 0;
        for (const auto& [image, sampled] : images) {
            infos.push_back({sampled ? sampler_ : VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = binding++;
            w.descriptorCount = 1;
            w.descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes.push_back(w);
        }
    };
    infos.reserve(kSets * 5);
    for (int p = 0; p < 2; p++) {  // p: B's parity; A is the other
        const int a = 1 - p;
        if (!make(kLuma, &luma_set_[p]) || !make(kWarp, &warp_set_[p])) return *why = "vkAllocateDescriptorSets", false;
        write(luma_set_[p], {{&frames_[p], true}, {&lum_[p][0], false}});
        write(warp_set_[p], {{&frames_[a], true}, {&frames_[p], true}, {&flows_[0][0], true}, {&flows_[1][0], true},
                             {&output_, false}});
        for (int l = 0; l < levels_; l++) {
            if (l > 0) {
                if (!make(kDown, &down_set_[p][l])) return *why = "vkAllocateDescriptorSets", false;
                write(down_set_[p][l], {{&lum_[p][l - 1], false}, {&lum_[p][l], false}});
            }
            for (int w = 0; w < 2; w++) {  // w 0: A to B (I0 = A, I1 = B); 1: B to A
                const Image& i0 = lum_[w == 0 ? a : p][l];
                const Image& i1 = lum_[w == 0 ? p : a][l];
                // (the coarsest has no coarser flow: its own is bound for the layout and the flag says not to read it)
                const Image& coarse = flows_[w][l + 1 < levels_ ? l + 1 : l];
                if (!make(kPatch, &patch_set_[p][w][l]) || !make(kDensify, &dense_set_[p][w][l]))
                    return *why = "vkAllocateDescriptorSets", false;
                write(patch_set_[p][w][l], {{&i0, false}, {&i1, false}, {&coarse, false}, {&patches_[w][l], false}});
                write(dense_set_[p][w][l], {{&patches_[w][l], false}, {&i0, false}, {&i1, false}, {&flows_[w][l], false}});
            }
        }
    }
    for (size_t k = 0; k < writes.size(); k++) writes[k].pImageInfo = &infos[k];  // (infos no longer grows)
    vkUpdateDescriptorSets(c->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

void Motion::record_init(VkCommandBuffer cmd) {
    std::vector<VkImageMemoryBarrier> b(all_.size());
    for (size_t k = 0; k < all_.size(); k++) {
        b[k] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b[k].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT;
        b[k].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[k].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[k].srcQueueFamilyIndex = b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[k].image = all_[k]->image;
        b[k].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(b.size()), b.data());
    initialized_ = true;
}

VkExtent2D Motion::grid(VkExtent2D level) {
    // Patches of 8 every 4 texels; the last one of a row or column ends at the edge (fg_patch clamps its origin)
    return {(std::max(level.width, 8u) - 8 + 3) / 4 + 1, (std::max(level.height, 8u) - 8 + 3) / 4 + 1};
}

void Motion::dispatch(VkCommandBuffer cmd, Pass pass, VkDescriptorSet set, VkExtent2D size, VkExtent2D src, int flags,
                      int extra, bool groups) const {
    const Pipeline& p = passes_[pass];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
    Push push{{static_cast<int32_t>(size.width), static_cast<int32_t>(size.height)},
              {static_cast<int32_t>(src.width), static_cast<int32_t>(src.height)},
              {1.0f / static_cast<float>(extent_.width), 1.0f / static_cast<float>(extent_.height)},
              flags,
              extra,
              time_};
    vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    if (groups) vkCmdDispatch(cmd, size.width, size.height, 1);
    else vkCmdDispatch(cmd, (size.width + 7) / 8, (size.height + 7) / 8, 1);
}

void Motion::record_shared(VkCommandBuffer cmd, uint64_t n) const {
    const int p = static_cast<int>(n % 2);
    dispatch(cmd, kLuma, luma_set_[p], lum_[p][0].extent, extent_, 0, static_cast<int>(finest_));
    for (int l = 1; l < levels_; l++) {
        compute_barrier(cmd);
        dispatch(cmd, kDown, down_set_[p][l], lum_[p][l].extent, lum_[p][l - 1].extent, 0, 0);
    }
}

void Motion::record_generate(VkCommandBuffer cmd, uint64_t n) const {
    const int p = static_cast<int>(n % 2);
    auto packed = [](VkExtent2D e) { return static_cast<int>(e.width | (e.height << 16)); };
    compute_barrier(cmd);  // (this frame's levels, from record_shared)
    // Light (the lowest quality, for weak GPUs): fewer search steps and bilinear samples in the warp, roughly a third
    // less work again
    const bool light = finest_ >= 4;
    for (int l = levels_ - 1; l >= 0; l--) {
        const bool coarsest = l == levels_ - 1;
        const VkExtent2D e = lum_[p][l].extent;
        // flags 1: start from the coarser level's flow; 2: search whole texels first (the coarsest)
        for (int w = 0; w < 2; w++)
            dispatch(cmd, kPatch, patch_set_[p][w][l], grid(e), e, (coarsest ? 2 : 1) | (light ? 4 : 0),
                     coarsest ? 0 : packed(flows_[w][l + 1].extent), true);
        compute_barrier(cmd);
        for (int w = 0; w < 2; w++) dispatch(cmd, kDensify, dense_set_[p][w][l], e, grid(e), 0, 0);
        compute_barrier(cmd);
    }
    record_again(cmd, n);
}

// Another picture between the same pair (adaptive frame generation): the flows are there already, only the warp
void Motion::record_again(VkCommandBuffer cmd, uint64_t n) const {
    dispatch(cmd, kWarp, warp_set_[n % 2], extent_, flows_[0][0].extent, finest_ >= 4 ? 0 : 1, static_cast<int>(finest_));
}

}  // namespace nvn::lsfg
