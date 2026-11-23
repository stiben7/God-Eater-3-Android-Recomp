#pragma once
// GPU-counted multi-draws on drivers without VK_KHR_draw_indirect_count (ARM's Vulkan 1.1 on a Dimensity 720): every
// draw up to max_draws is issued, one call each, and the vertex shader drops the ones past the count the game's
// culling wrote. It reads that count as the draw runs, from the same place a driver with draw-indirect-count would, so
// nothing has to be copied or fixed before the pass. A dropped draw puts every vertex at (0, 0, 0, 0): nothing is
// rasterized.
//
// The vertex shader gets, besides what Eden gave it:
// - set 2, binding 0: a storage buffer of uints, the count's word among them (kCountCheckSet);
// - push constants: { uint draw; uint word; } (this draw's index in the multi-draw, where the count is).

#include <cstdint>
#include <vector>

namespace nvn {

constexpr uint32_t kCountCheckSet = 2;
constexpr uint32_t kCountCheckPushSize = 8;

// Adds the check to a vertex shader's SPIR-V (in place). false: not added (no gl_Position written as a plain output
// variable, or SPIR-V it doesn't understand); w is then unchanged.
bool add_count_check(std::vector<uint32_t>& w);

}  // namespace nvn
