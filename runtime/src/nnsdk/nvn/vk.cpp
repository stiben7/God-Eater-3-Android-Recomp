#include "nnsdk/nvn/vk.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

#include "nnsdk/nvn/driver.h"
#include "nnsdk/nvn/nvn.h"
#include "nnsdk/ini.h"

namespace nvn {

const char* vk_result_name(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_NOT_READY: return "VK_NOT_READY";
        default: return "VkResult desconocido";
    }
}

namespace {

// DEPURACIÓN (NIER_SONDA_MEMORIA): cuántas imágenes y cuántas reservas aguanta el driver antes de fallar.
// Las imágenes van todas en una misma reserva, como con gpu_memory.cpp.
void probe_limits(VkContext& c) {
    uint32_t type = 0;
    for (uint32_t k = 0; k < c.memory.memoryTypeCount; k++)
        if (c.memory.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            type = k;
            break;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = 256ull << 20;
    ai.memoryTypeIndex = type;
    VkDeviceMemory block = VK_NULL_HANDLE;
    vkAllocateMemory(c.device, &ai, nullptr, &block);
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkDeviceSize offset = 0;
    const char* fail = "ninguno";
    for (int n = 0; n < 6000 && block; n++) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {64, 64, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage img;
        if (vkCreateImage(c.device, &ii, nullptr, &img) != VK_SUCCESS) { fail = "vkCreateImage"; break; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(c.device, img, &req);
        offset = (offset + req.alignment - 1) / req.alignment * req.alignment;
        if (offset + req.size > ai.allocationSize || vkBindImageMemory(c.device, img, block, offset) != VK_SUCCESS) {
            vkDestroyImage(c.device, img, nullptr);
            fail = "sin sitio o vkBindImageMemory";
            break;
        }
        offset += req.size;
        images.push_back(img);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        if (vkCreateImageView(c.device, &vi, nullptr, &view) != VK_SUCCESS) { fail = "vkCreateImageView"; break; }
        views.push_back(view);
    }
    NVN_LOG("sonda: %zu imágenes con vista en una reserva (falla: %s)\n", images.size(), fail);
    for (VkImageView v : views) vkDestroyImageView(c.device, v, nullptr);
    for (VkImage i : images) vkDestroyImage(c.device, i, nullptr);
    if (block) vkFreeMemory(c.device, block, nullptr);
    std::vector<VkDeviceMemory> mems;
    ai.allocationSize = 64 * 1024;
    for (int n = 0; n < 6000; n++) {
        VkDeviceMemory m;
        if (vkAllocateMemory(c.device, &ai, nullptr, &m) != VK_SUCCESS) break;
        mems.push_back(m);
    }
    NVN_LOG("sonda: %zu reservas de 64 KB\n", mems.size());
    for (VkDeviceMemory m : mems) vkFreeMemory(c.device, m, nullptr);
}

// Driver environment: <datos>/driver_env.txt, one NAME=value per line (# starts a comment), set before the driver
// is loaded. Mesa's drivers read their debug settings from the environment when the Vulkan instance is created:
// TU_DEBUG=sysmem,nolrz,noubwc and FD_DEV_FEATURES=… for Turnip (still being brought up on the Adreno 8xx),
// PAN_DEBUG / PANVK_DEBUG for PanVK. The settings menu writes it (Debug → Driver environment). Read at start-up.
void driver_environment() {
    // "env.NAME = value" lines of mhr.ini (Debug → Driver environment), set before the driver loads.
    for (const auto& [key, value] : nnsdk::ini::read()) {
        if (key.compare(0, 4, "env.") != 0 || key.size() == 4) continue;
        setenv(key.c_str() + 4, value.c_str(), 1);
        NVN_LOG("driver environment: %s=%s\n", key.c_str() + 4, value.c_str());
    }
}

bool create(VkContext& c, bool system_driver) {
    driver_environment();
    if (!driver::load(system_driver)) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MH Rise port";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instance_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instance_ext;
    if (VkResult r = vkCreateInstance(&ici, nullptr, &c.instance); r != VK_SUCCESS) {
        NVN_LOG("vkCreateInstance: %s\n", vk_result_name(r));
        return false;
    }
    driver::load_instance(c.instance);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
    if (!count) {
        NVN_LOG("no hay GPU Vulkan\n");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(c.instance, &count, gpus.data());
    c.gpu = gpus[0];
    vkGetPhysicalDeviceProperties(c.gpu, &c.props);
    {
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &c.subgroup;
        vkGetPhysicalDeviceProperties2(c.gpu, &p2);
        NVN_LOG("subgrupos: tamaño %u, etapas 0x%x, operaciones 0x%x\n", c.subgroup.subgroupSize,
                c.subgroup.supportedStages, c.subgroup.supportedOperations);
    }
    vkGetPhysicalDeviceMemoryProperties(c.gpu, &c.memory);
    NVN_LOG("GPU: %s, Vulkan %u.%u.%u, driver 0x%x\n", c.props.deviceName, VK_VERSION_MAJOR(c.props.apiVersion),
            VK_VERSION_MINOR(c.props.apiVersion), VK_VERSION_PATCH(c.props.apiVersion), c.props.driverVersion);

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(c.gpu, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(c.gpu, nullptr, &ext_count, exts.data());
    for (const char* wanted : {"VK_EXT_external_memory_host", "VK_EXT_vertex_attribute_divisor",
                               "VK_EXT_custom_border_color", "VK_EXT_descriptor_indexing", "VK_EXT_sampler_filter_minmax",
                               "VK_EXT_index_type_uint8",
                               "VK_EXT_extended_dynamic_state", "VK_KHR_imageless_framebuffer",
                               "VK_EXT_depth_clip_enable", "VK_KHR_dynamic_rendering",
                               VK_QCOM_RENDER_PASS_STORE_OPS_EXTENSION_NAME}) {
        bool found = false;
        for (const VkExtensionProperties& e : exts) found |= strcmp(e.extensionName, wanted) == 0;
        NVN_LOG("extensión %s: %s\n", wanted, found ? "sí" : "no");
    }

    vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(c.gpu, &count, families.data());
    c.queue_family = UINT32_MAX;
    for (uint32_t k = 0; k < count && c.queue_family == UINT32_MAX; k++)
        if (families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT && families[k].queueFlags & VK_QUEUE_COMPUTE_BIT)
            c.queue_family = k;
    if (c.queue_family == UINT32_MAX) {
        NVN_LOG("no hay cola gráfica\n");
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = c.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    std::vector<const char*> device_ext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    bool draw_indirect_count = false;
    // Opcionales: STORE_OP_NONE (Qualcomm) para no guardar los adjuntos que un pase no escribe (ver el ejecutor).
    for (const VkExtensionProperties& e : exts)
        if (strcmp(e.extensionName, VK_QCOM_RENDER_PASS_STORE_OPS_EXTENSION_NAME) == 0) {
            device_ext.push_back(VK_QCOM_RENDER_PASS_STORE_OPS_EXTENSION_NAME);
            c.store_op_none = true;
        } else if (strcmp(e.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0) {
            device_ext.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
            c.display_timing = true;
        } else if (strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0) {
            device_ext.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
            c.push_descriptor = true;
        } else if (strcmp(e.extensionName, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME) == 0) {
            device_ext.push_back(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
            draw_indirect_count = true;
        }
    // Todo lo que soporte el driver (ASTC y BCn para las texturas del juego, lo que pidan los shaders
    // traducidos…), menos robustBufferAccess, que cuesta rendimiento y el juego no lo necesita.
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(c.gpu, &features);
    features.robustBufferAccess = VK_FALSE;
    c.depth_bias_clamp = features.depthBiasClamp == VK_TRUE;
    c.multi_viewport = features.multiViewport == VK_TRUE;
    c.multi_draw_indirect = features.multiDrawIndirect == VK_TRUE;
    c.tessellation = features.tessellationShader == VK_TRUE;
    c.fill_non_solid = features.fillModeNonSolid == VK_TRUE;
    // robustBufferAccess: a read past the end of a buffer gives 0 (as Maxwell's constant buffers do) instead of whatever
    // memory is there. Adreno shrugs that off; an ARM Mali takes it as a GPU page fault (GROUP_ERROR_FATAL in the log,
    // status ...02c2: a translation fault), drops the whole batch of work, uploads included, and the picture goes black
    // or magenta (ASTC that never arrived) until something rebuilds it. So on by default on Mali (vendor 0x13B5);
    // <datos>/no_robust turns it off there, and <datos>/robusto turns it on anywhere. Read at start-up.
    const bool arm = c.props.vendorID == 0x13B5;
    bool robust = false;
    if (nnsdk::ini::on("robusto") || (arm && !nnsdk::ini::on("no_robust"))) {
        features.robustBufferAccess = VK_TRUE;
        robust = true;
        NVN_LOG("robustBufferAccess: on (%s)\n", arm ? "Mali" : "robusto");
    }
    NVN_LOG("límites: salidas de vértice %u, entradas de fragmento %u, geometry entradas %u salidas %u vértices %u\n",
            c.props.limits.maxVertexOutputComponents, c.props.limits.maxFragmentInputComponents,
            c.props.limits.maxGeometryInputComponents, c.props.limits.maxGeometryOutputComponents,
            c.props.limits.maxGeometryOutputVertices);
    NVN_LOG("memoria: máx. %u reservas, granularidad buffer/imagen %llu\n", c.props.limits.maxMemoryAllocationCount,
            static_cast<unsigned long long>(c.props.limits.bufferImageGranularity));
    for (uint32_t k = 0; k < c.memory.memoryHeapCount; k++)
        NVN_LOG("memoria: heap %u de %llu MB, flags 0x%x\n", k, static_cast<unsigned long long>(c.memory.memoryHeaps[k].size >> 20),
                c.memory.memoryHeaps[k].flags);
    NVN_LOG("features: ASTC %d, BC %d, ETC2 %d, geometry %d, tess %d, anisotropía %d, escrituras en vértice %d y fragmento %d\n",
            features.textureCompressionASTC_LDR, features.textureCompressionBC, features.textureCompressionETC2,
            features.geometryShader, features.tessellationShader, features.samplerAnisotropy,
            features.vertexPipelineStoresAndAtomics, features.fragmentStoresAndAtomics);
    // For the settings menu (SettingsMenu: GpuInfo), which shows the options of this GPU only
    if (FILE* g = fopen((nnsdk::ini::data_dir() + "/gpu.txt").c_str(), "w")) {
        auto has_ext = [&](const char* name) {
            for (const VkExtensionProperties& e : exts)
                if (strcmp(e.extensionName, name) == 0) return 1;
            return 0;
        };
        fprintf(g, "vendor 0x%x\nbc %d\nastc %d\nname %s\neds %d\nvids %d\n", c.props.vendorID,
                features.textureCompressionBC ? 1 : 0, features.textureCompressionASTC_LDR ? 1 : 0, c.props.deviceName,
                has_ext(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME), has_ext(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME));
        fclose(g);
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &features;
    // shaderFloat16 (solo eso de VK_KHR_shader_float16_int8): lo piden los shaders FP16 de LSFG (lsfg/); sin él,
    // LSFG usa los FP32. El resto del dispositivo no cambia.
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR float16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR};
    for (const VkExtensionProperties& e : exts)
        if (strcmp(e.extensionName, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME) == 0) {
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &float16;
            vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
        }
    if (float16.shaderFloat16) {
        float16.shaderInt8 = VK_FALSE;
        dci.pNext = &float16;
        device_ext.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    }
    // Con <datos>/robusto, también robustBufferAccess2 (VK_EXT_robustness2): los índices leídos fuera del buffer de
    // índices (robustBufferAccess no los cubre) dan 0 en vez de un fallo de página.
    VkPhysicalDeviceRobustness2FeaturesEXT robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    if (robust)
        for (const VkExtensionProperties& e : exts)
            if (strcmp(e.extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) {
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &robust2;
                vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
                robust2.nullDescriptor = VK_FALSE;
                robust2.robustImageAccess2 = VK_FALSE;
                if (robust2.robustBufferAccess2) {
                    robust2.pNext = const_cast<void*>(dci.pNext);
                    dci.pNext = &robust2;
                    device_ext.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                    NVN_LOG("robustBufferAccess2: sí\n");
                }
            }
    // Extended dynamic state: the rasterizer and depth/stencil state the game changes most often, set per draw. A game
    // state that differs only in those (cull mode, which face is front, depth test/write/compare, the stencil test and
    // its ops) shares one pipeline instead of one each: fewer pipelines to build (the hitches when something new shows
    // up) and fewer to switch between. Off by default (drivers have had bugs with it): <datos>/dynamic_state at start-up
    // (Settings → Graphics, where the driver has it).
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    if (nnsdk::ini::on("dynamic_state"))
        for (const VkExtensionProperties& e : exts)
            if (strcmp(e.extensionName, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &eds;
                vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
                if (eds.extendedDynamicState) {
                    eds.pNext = const_cast<void*>(dci.pNext);
                    dci.pNext = &eds;
                    device_ext.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
                    c.extended_dynamic_state = true;
                }
            }
    NVN_LOG("extended dynamic state: %s\n", c.extended_dynamic_state ? "on" : "off");
    // Vertex input dynamic state: the attributes' formats and the streams' strides and step rates per draw as well, so
    // a mesh with another vertex layout doesn't make a pipeline of its own. Off by default: <datos>/vertex_input_state at
    // start-up (Settings → Graphics, where the driver has it).
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    if (nnsdk::ini::on("vertex_input_state"))
        for (const VkExtensionProperties& e : exts)
            if (strcmp(e.extensionName, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME) == 0) {
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                f2.pNext = &vids;
                vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
                if (vids.vertexInputDynamicState) {
                    vids.pNext = const_cast<void*>(dci.pNext);
                    dci.pNext = &vids;
                    device_ext.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
                    c.vertex_input_dynamic_state = true;
                }
            }
    NVN_LOG("vertex input dynamic state: %s\n", c.vertex_input_dynamic_state ? "on" : "off");
    // gl_DrawID, gl_BaseVertex y gl_BaseInstance en los shaders traducidos (el DrawID de los multi-draw).
    VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &draw_parameters;
        vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
        draw_parameters.pNext = const_cast<void*>(dci.pNext);
        if (draw_parameters.shaderDrawParameters) dci.pNext = &draw_parameters;
    }
    NVN_LOG("shaderDrawParameters: %s, draw_indirect_count: %s\n", draw_parameters.shaderDrawParameters ? "sí" : "no",
            draw_indirect_count ? "sí" : "no");
    // Punteros variables a storage buffers (los bucles CAS de Eden que sí se llaman: un puntero como parámetro). Antes no
    // se activaban y los shaders que declaran la capacidad no eran válidos; el driver de Qualcomm los rechazaba.
    VkPhysicalDeviceVariablePointersFeatures variable_pointers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &variable_pointers;
        vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
        variable_pointers.pNext = const_cast<void*>(dci.pNext);
        if (variable_pointers.variablePointersStorageBuffer) dci.pNext = &variable_pointers;
        NVN_LOG("variablePointersStorageBuffer: %s\n", variable_pointers.variablePointersStorageBuffer ? "yes" : "no");
    }
    // Bindless (arrays de texturas con índice por píxel), bordes exactos y reducción MIN/MAX: solo lo que se usa.
    auto has_ext = [&](const char* name) {
        for (const VkExtensionProperties& e : exts)
            if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT indexing_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT};
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT};
    if (has_ext(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &indexing_query;
        vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
        NVN_LOG("descriptor_indexing: no uniforme (texturas) %d, runtimeDescriptorArray %d, partiallyBound %d, "
                "updateAfterBind (texturas) %d, variableDescriptorCount %d\n",
                indexing_query.shaderSampledImageArrayNonUniformIndexing, indexing_query.runtimeDescriptorArray,
                indexing_query.descriptorBindingPartiallyBound, indexing_query.descriptorBindingSampledImageUpdateAfterBind,
                indexing_query.descriptorBindingVariableDescriptorCount);
        if (indexing_query.shaderSampledImageArrayNonUniformIndexing) {
            indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            indexing.pNext = const_cast<void*>(dci.pNext);
            dci.pNext = &indexing;
            device_ext.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
            c.nonuniform_sampled = true;
        }
    }
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    if (has_ext(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &border;
        vkGetPhysicalDeviceFeatures2(c.gpu, &f2);
        VkPhysicalDeviceCustomBorderColorPropertiesEXT border_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &border_props;
        vkGetPhysicalDeviceProperties2(c.gpu, &p2);
        NVN_LOG("custom_border_color: %d, sin formato %d, hasta %u samplers\n", border.customBorderColors,
                border.customBorderColorWithoutFormat, border_props.maxCustomBorderColorSamplers);
        if (border.customBorderColors && border.customBorderColorWithoutFormat) {
            border.pNext = const_cast<void*>(dci.pNext);
            dci.pNext = &border;
            device_ext.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
            c.custom_border_color = true;
            c.max_custom_border_samplers = border_props.maxCustomBorderColorSamplers;
        }
    }
    if (has_ext(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME)) {
        device_ext.push_back(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
        c.filter_minmax = true;
    }
    NVN_LOG("shaderFloat16: %s\n", float16.shaderFloat16 ? "sí" : "no");
    dci.enabledExtensionCount = static_cast<uint32_t>(device_ext.size());
    dci.ppEnabledExtensionNames = device_ext.data();
    VkResult r = vkCreateDevice(c.gpu, &dci, nullptr, &c.device);
    if (r != VK_SUCCESS && device_ext.size() > 1) {  // sin las opcionales, antes que no arrancar
        NVN_LOG("vkCreateDevice con extensiones opcionales: %s; sin ellas\n", vk_result_name(r));
        c.store_op_none = c.display_timing = c.push_descriptor = false;
        c.nonuniform_sampled = c.custom_border_color = c.filter_minmax = false;
        float16.shaderFloat16 = VK_FALSE;
        draw_indirect_count = false;
        dci.pNext = nullptr;
        dci.enabledExtensionCount = 1;
        r = vkCreateDevice(c.gpu, &dci, nullptr, &c.device);
    }
    if (r != VK_SUCCESS) {
        NVN_LOG("vkCreateDevice: %s\n", vk_result_name(r));
        return false;
    }
    vkGetDeviceQueue(c.device, c.queue_family, 0, &c.queue);
    driver::check();
    c.shader_float16 = float16.shaderFloat16;
    if (draw_indirect_count) {
        c.draw_indexed_indirect_count = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(
            vkGetDeviceProcAddr(c.device, "vkCmdDrawIndexedIndirectCountKHR"));
        c.draw_indirect_count = reinterpret_cast<PFN_vkCmdDrawIndirectCountKHR>(vkGetDeviceProcAddr(c.device, "vkCmdDrawIndirectCountKHR"));
    }
    if (const char* probe = getenv("NIER_SONDA_MEMORIA"); probe && *probe) probe_limits(c);
    return true;
}

}  // namespace

VkContext* vk() {
    static VkContext context;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        ok = create(context, false);
        if (!ok && driver::custom()) {  // con el driver del usuario no arrancó: el del sistema, en este mismo arranque
            driver::give_up("could not create the instance, GPU or device");
            if (context.device) vkDestroyDevice(context.device, nullptr);
            if (context.instance) vkDestroyInstance(context.instance, nullptr);
            context = VkContext{};
            ok = create(context, true);
        }
    });
    return ok ? &context : nullptr;
}

}  // namespace nvn
