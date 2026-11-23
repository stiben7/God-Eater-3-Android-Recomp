#pragma once
// The port's own frame generator: no Lossless.dll, nothing past Vulkan 1.1 (compute shaders, storage images of the
// formats every driver has: R32F, RGBA32F, RGBA8). What framegen.cpp uses when there's no DLL, or when Lossless's
// chain can't be built on this GPU (an old driver), and the menu's choice for it.
//
// Dense optical flow by inverse search (DIS: Kroeger, Timofte, Dai and Van Gool, "Fast Optical Flow using Dense Inverse
// Search", ECCV 2016), both ways, and a warp that knows what's covered and uncovered:
//   per real frame n (record_shared): its luma at four levels from 1/4 of the picture (Light; 1/3 Balanced, 1/2 Sharp)
//   down, kept for the next pair;
//   for the picture between n - 1 (A) and n (B) (record_generate), from the coarsest level to the finest, A to B and
//   B to A:
//     1. patches of 8x8 texels every 4 (fg_patch): each starts from the best of the coarser level's motion around it
//        and none (on the coarsest, the best whole-texel offset within +-4), then a few steps of Lucas-Kanade against
//        the template's own gradients (the inverse search) put it below a texel;
//     2. a vector per texel (fg_densify): the patches that cover it, each weighted by how well it fits right there;
//   then each pixel of the picture between (fg_warp) follows both fields back to where it was in A and in B; a vector
//   that the other field doesn't send back (forward-backward check) marks something covered or uncovered, and the
//   colour comes from the frame that sees it. Where both agree, A and B averaged (bicubic samples, as sharp as the real
//   frames); a pixel the same in both (the HUD, still scenery) keeps B's.
// Shaders: shaders/fg_*.comp (tools/shaders.sh). A build whose spirv.h doesn't have them yet can't make one
// (available() false): the presenter goes on with LSFG alone.

#include <string>
#include <vector>

#include "nnsdk/nvn/lsfg/generator.h"

namespace nvn::lsfg {

class Motion final : public Generator {
public:
    static bool available();

    Motion() = default;
    Motion(const Motion&) = delete;
    Motion& operator=(const Motion&) = delete;
    ~Motion() override { destroy(); }

    // For a picture of that size and format (the window's). false with the reason (and nothing made) if it fails.
    // flow: the quality, as LSFG's flow scale (0.25 Light, 0.5 Balanced, 1 Sharp)
    bool create(VkExtent2D extent, VkFormat format, float flow, std::string* why);
    void destroy();

    const char* name() const override { return "built-in"; }
    bool initialized() const override { return initialized_; }
    void record_init(VkCommandBuffer cmd) override;
    const Image& input(uint64_t n) const override { return frames_[n % 2]; }
    const Image& output() const override { return output_; }
    void record_shared(VkCommandBuffer cmd, uint64_t n) const override;
    void record_generate(VkCommandBuffer cmd, uint64_t n) const override;
    bool set_time(float t) override {
        time_ = t;
        return true;
    }
    void record_again(VkCommandBuffer cmd, uint64_t n) const override;
    VkExtent2D extent() const override { return extent_; }
    VkFormat format() const override { return format_; }
    float flow() const override { return flow_; }
    size_t images() const override { return all_.size(); }
    VkDeviceSize bytes() const override { return bytes_; }
    uint32_t sets() const override { return kSets; }

private:
    enum Pass { kLuma, kDown, kPatch, kDensify, kWarp, kPasses };
    static constexpr int kLevels = 4;  // from the finest (1/2, 1/3 or 1/4 of the picture) down, halving (fewer if small)
    struct Pipeline {
        VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    // Per parity of B's frame (n % 2): luma, the downs (level 1 up), and per way (0: A to B, 1: B to A) and level the
    // patch and densify passes; and the warp
    static constexpr uint32_t kSets = 2 * (1 + kLevels + 2 * kLevels * 2 + 1);

    void add(Image& image, VkExtent2D extent, VkFormat format, VkFormat view_format, VkImageUsageFlags usage,
             VkImageCreateFlags flags = 0);
    bool build_pipelines(std::string* why);
    bool build_images(std::string* why);
    bool build_sets(std::string* why);
    // size: the output's (a thread each), or with groups the number of 8x8 groups (fg_patch: one per patch)
    void dispatch(VkCommandBuffer cmd, Pass pass, VkDescriptorSet set, VkExtent2D size, VkExtent2D src, int flags,
                  int extra, bool groups = false) const;
    static VkExtent2D grid(VkExtent2D level);  // patches of a level

    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    int levels_ = kLevels;
    float flow_ = 0.25f;
    float time_ = 0.5f;  // the next warp's place between A and B (set_time)
    uint32_t finest_ = 4;  // picture pixels per texel of the finest flow level
    bool initialized_ = false;
    Pipeline passes_[kPasses];
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet luma_set_[2]{}, down_set_[2][kLevels]{}, patch_set_[2][2][kLevels]{}, dense_set_[2][2][kLevels]{},
        warp_set_[2]{};
    std::vector<Image*> all_;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize bytes_ = 0;

    Image frames_[2], lum_[2][kLevels];             // the pictures and their luma levels, by parity
    Image patches_[2][kLevels], flows_[2][kLevels];  // by way (A to B, B to A) and level
    Image output_;
};

}  // namespace nvn::lsfg
