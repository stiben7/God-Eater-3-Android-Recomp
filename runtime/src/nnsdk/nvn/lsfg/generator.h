#pragma once
// What the presenter (framegen.cpp) needs from a frame generator: Lossless Scaling's chain (chain.h, with the player's
// Lossless.dll) or the port's own (motion.h, no DLL, plain Vulkan 1.1 compute). For real frame n the presenter copies
// the window's picture into input(n), records the shared work of n, and with n - 1 there too, the picture between
// them into output().

#include <cstdint>

#include "nnsdk/nvn/vk.h"

namespace nvn::lsfg {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED, view_format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags flags = 0;
    VkDeviceSize offset = 0;
};

class Generator {
public:
    virtual ~Generator() = default;
    virtual const char* name() const = 0;
    virtual bool initialized() const = 0;
    virtual void record_init(VkCommandBuffer cmd) = 0;
    virtual const Image& input(uint64_t n) const = 0;
    virtual const Image& output() const = 0;
    virtual void record_shared(VkCommandBuffer cmd, uint64_t n) const = 0;
    virtual void record_generate(VkCommandBuffer cmd, uint64_t n) const = 0;
    // Adaptive frame generation (more than one picture between a pair): where the next picture goes, from 0 (A) to 1
    // (B); 0.5 until it's set. Takes effect in what's recorded after it, and with LSFG it rewrites the chain's
    // constants, so the GPU has to be done with the last generation first. false: this generator can't.
    virtual bool set_time(float t) = 0;
    // One more picture for the pair record_generate did (its shared work and flows are kept), at the time set
    virtual void record_again(VkCommandBuffer cmd, uint64_t n) const = 0;
    virtual VkExtent2D extent() const = 0;
    virtual VkFormat format() const = 0;
    virtual float flow() const = 0;
    virtual size_t images() const = 0;
    virtual VkDeviceSize bytes() const = 0;
    virtual uint32_t sets() const = 0;
};

}  // namespace nvn::lsfg
