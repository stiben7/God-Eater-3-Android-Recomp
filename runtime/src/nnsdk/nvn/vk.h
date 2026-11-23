#pragma once
// Contexto Vulkan único detrás de NVN: instancia, GPU, dispositivo y cola gráfica.

// (VK_USE_PLATFORM_ANDROID_KHR y VK_NO_PROTOTYPES vienen de CMake: las vk* son los punteros de volk, que llena driver.cpp)
#include <volk.h>

namespace nvn {

struct VkContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;
    // VK_QCOM_render_pass_store_ops activa: los render passes pueden usar VK_ATTACHMENT_STORE_OP_NONE.
    bool store_op_none = false;
    // VK_GOOGLE_display_timing activa: se puede pedir a qué hora se muestra cada imagen (LSFG: el real 16,7 ms
    // después del generado, para que el compositor no tire uno de los dos).
    bool display_timing = false;
    bool push_descriptor = false;  // VK_KHR_push_descriptor activa (el escalado FSR, fsr.cpp)
    bool shader_float16 = false;  // VK_KHR_shader_float16_int8 encendida: los shaders FP16 de LSFG (lsfg/)
    // VK_EXT_descriptor_indexing con shaderSampledImageArrayNonUniformIndexing: los arrays bindless (decals, sondas) se
    // indexan por píxel con NonUniform (nvnshader::Options::nonuniform_indexing).
    bool nonuniform_sampled = false;
    // VK_EXT_custom_border_color (sin formato) y VK_EXT_sampler_filter_minmax: bordes exactos y reducción MIN/MAX.
    bool custom_border_color = false, filter_minmax = false;
    bool multi_viewport = true;
    // VK_EXT_extended_dynamic_state on (vk.cpp): cull mode, front face, depth test/write/compare and the stencil test
    // and ops are set per draw instead of baked into the pipeline (executor.cpp), so far fewer pipelines
    bool extended_dynamic_state = false;
    // VK_EXT_vertex_input_dynamic_state on: the vertex attributes' formats and the streams' strides and step rates set
    // per draw too (executor.cpp)
    bool vertex_input_dynamic_state = false;
    bool depth_bias_clamp = false;  // the depthBiasClamp feature (without it the clamp of a depth bias must be 0)  // la feature multiViewport (vk.cpp): los shaders que la usan, sin ella si no está
    uint32_t max_custom_border_samplers = 0;
    // VK_KHR_draw_indirect_count: los multi-draw de NVN con el número de draws en la GPU (nulos sin la extensión).
    PFN_vkCmdDrawIndexedIndirectCountKHR draw_indexed_indirect_count = nullptr;
    PFN_vkCmdDrawIndirectCountKHR draw_indirect_count = nullptr;
    bool tessellation = false;         // the tessellationShader feature
    bool multi_draw_indirect = false;  // the multiDrawIndirect feature: more than one draw per indirect call
    bool fill_non_solid = true;        // the fillModeNonSolid feature: point and line polygon modes
};

// Se crea en la primera llamada; nullptr si Vulkan no arranca.
VkContext* vk();

const char* vk_result_name(VkResult r);

}  // namespace nvn
