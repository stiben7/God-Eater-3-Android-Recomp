// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// LSFG 2x en el presentador: nuestra versión de frame_gen.cpp y frame_gen_pacer.cpp de Eden (la copia del frame
// a un anillo de 2 entradas, el calentamiento, el apagado por carga con reintentos 5/15/30/60 s).
//
// Por frame real N, con LSFG encendido y listo (todo en el hilo que presenta, bajo vk_queue_mutex()):
//   1. un envío propio: copia exacta de la textura de la ventana a input(N), las pasadas compartidas de N y,
//      con la historia ya llena, las de generación: G(N-1, N) en la imagen de salida de la cadena (1280x720);
//   2. si hay G: blit de la salida a una imagen del swapchain y vkQueuePresentKHR;
//   3. window.cpp presenta N como siempre (blit de la textura del juego), justo después, sin dormir.
// FIFO pone uno por vsync: G y luego N, dos por cada 33,3 ms. El ritmo del juego no cambia (una llamada a
// nvn::present por frame real), así que su lógica sigue a 30.
//
// Interruptores (flags.h): <datos>/lsfg enciende;
// <datos>/lsfg_flow_50 / lsfg_flow_100 el flujo óptico a la mitad / entero (por defecto un cuarto: Quality en la pantalla
// del driver); <datos>/lsfg_fp32 fuerza
// los shaders FP32 (se mira al compilar, una vez por sesión).

#include "nnsdk/nvn/lsfg/framegen.h"

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nnsdk/nvn/commands.h"
#include "nnsdk/nvn/flags.h"
#include "nnsdk/nvn/fsr.h"
#include "nnsdk/nvn/hud.h"
#include "nnsdk/nvn/lsfg/chain.h"
#include "nnsdk/nvn/lsfg/dll.h"
#include "nnsdk/nvn/lsfg/motion.h"
#include "nnsdk/nvn/nvn.h"
#include "nnsdk/nvn/objects.h"

namespace nvn::lsfg {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSlots = 3;                        // envíos de LSFG en vuelo: uno por frame real
constexpr uint64_t kSlotWaitNs = 250'000'000;    // el de hace 3 frames sin acabar en 250 ms: este frame sin LSFG
constexpr uint64_t kAcquireNs = 100'000'000;
constexpr uint64_t kRetireWaitNs = 50'000'000;   // al apagar: espera acotada antes de soltar la cadena
constexpr uint64_t kWarmFrames = 3;              // se genera desde el 4.º frame seguido: beta lee 3 de historia
constexpr double kHitchMs = 100.0;               // un hueco así (carga, tirón): no se interpola a través de él
constexpr double kTooFastMs = 25.0;              // el juego presenta a más de ~40 fps: no cabe un frame generado

double ms_between(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::string data_path(const char* name) {
    const char* data = getenv("NIER_DATA");
    return std::string(data ? data : ".") + "/" + name;
}

// Adaptive frame generation (Settings → Frame generation → Target, fg_target_60/90/120): up to kMaxExtra more pictures
// between a pair, each its own submit and present, after the first (the Slot's own)
constexpr int kMaxExtra = 2;
constexpr int kTargetFrames = 15;  // real frames a new multiplier has to hold before it's used (no flip-flopping)
struct Extra {
    VkCommandBuffer cmd = VK_NULL_HANDLE, blit = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    bool pending = false;
};

struct Slot {
    Extra extra[kMaxExtra];
    VkCommandBuffer cmd = VK_NULL_HANDLE, blit = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    bool pending = false, timed = false;
    const Generator* chain = nullptr;  // la cadena que usa este envío (no se suelta hasta que acabe)
};

class FrameGen {
public:
    uint32_t swapchain_images(const VkSurfaceCapabilitiesKHR& caps);
    void present(Window& w, Texture* t);
    uint64_t generated() const { return total_generated_; }  // desde el arranque (las estadísticas se reinician)
    // Lo que le cuesta a la GPU un frame generado (media móvil, ns) mientras LSFG está encendido; 0 apagado.
    int64_t reserved_ns() const {
        return enabled_ ? std::max<int64_t>(cost_ns_.load(std::memory_order_relaxed), 6000000) * (multiplier_ - 1) : 0;
    }
    int64_t spacing_ns() const { return spacing_ns_; }
    int64_t last_present_ns() const { return last_present_ns_; }
    uint32_t last_id() const { return last_id_; }

private:
    uint32_t last_id_ = 0;
    std::atomic<int64_t> cost_ns_{0};
    uint64_t total_generated_ = 0;
    int64_t last_present_ns_ = 0;
    void disable(const std::string& why);
    bool setup();
    void collect(int index);
    void poll_slots();
    void retire();
    void reap(bool wait);
    void start_job(VkExtent2D extent, VkFormat format, float flow);
    void poll_job();
    bool compile(std::string* why, bool* missing);
    void log_stats(Clock::time_point now);
    bool record_lsfg(int index, const Texture& t, const VkRect2D& crop, uint64_t n, bool generate);
    bool record_blit(VkCommandBuffer blit, const Window& w, uint32_t image);
    bool wait_generations(uint64_t timeout_ns);
    int64_t refresh_ns(const Window& w);
    void present_extras(Window& w, Slot& s, uint64_t n, int m, int64_t first_ns);
    int multiplier_ = 2, wanted_ = 2, wanted_frames_ = 0;  // pictures per real frame, the real one included
    float chain_time_ = 0.5f;
    int64_t spacing_ns_ = 16666667, refresh_ns_ = 0;
    VkSwapchainKHR refresh_of_ = VK_NULL_HANDLE;

    enum class State { Off, Waiting, Ready, Failed };
    State state_ = State::Off;  // Waiting: compilando o montando la cadena en el hilo de fondo
    bool enabled_ = false, missing_dll_ = false, few_images_logged_ = false;

    // Hilo de fondo: compila (la primera vez) y monta una cadena; el hilo que presenta recoge el resultado.
    struct Job {
        std::mutex m;
        bool busy = false, done = false, fatal = false, missing = false, fell_back = false;
        Generator* chain = nullptr;  // sin unique_ptr: un resultado sin recoger no se destruye al salir del proceso
        std::string why;
        double ms = 0;
    } job_;
    Pipelines pipelines_;     // los escribe solo el hilo de fondo, antes de la primera cadena
    bool compiled_ = false;   // ídem; se lee con el trabajo ya terminado
    const char* variant_ = "";
    // The built-in generator (motion.h) instead of LSFG: chosen (<datos>/fg_builtin), or LSFG couldn't run here (no DLL,
    // or its chain can't be built on this GPU), which holds until the game restarts
    bool lsfg_unusable_ = false;
    bool want_builtin() const { return lsfg_unusable_ || NVN_FLAG("fg_builtin"); }

    Generator* chain_ = nullptr;    // la que se usa
    Generator* retired_ = nullptr;  // apagada o reconstruida: se suelta cuando la GPU acaba sus envíos
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    uint64_t timestamp_mask_ = 0;
    Slot slots_[kSlots];
    int next_ = 0;
    bool set_up_ = false;

    uint64_t warm_ = 0;  // frames seguidos copiados a la cadena (0: hay que volver a calentar)
    Clock::time_point last_{}, hold_until_{};
    double ema_ = 0;
    bool pace_logged_ = false;

    // Contadores de los ~5 s del log.
    Clock::time_point stats_start_{};
    uint32_t real_ = 0, generated_ = 0, warming_ = 0, skip_hold_ = 0, skip_pace_ = 0, skip_busy_ = 0,
             skip_image_ = 0;
    double gpu_sum_ = 0, gpu_max_ = 0;
    uint32_t gpu_n_ = 0;
    bool announced_ = false;
};

FrameGen g_framegen;

uint32_t FrameGen::swapchain_images(const VkSurfaceCapabilitiesKHR& caps) {
    uint32_t n = 3;
    const std::string dll = find_dll();
    if (!dll.empty()) {
        n = 4 + kMaxExtra;
        NVN_LOG("lsfg: hay %s: swapchain de %u imágenes (los frames generados y el real van seguidos)\n", dll.c_str(), n);
    } else if (Motion::available()) {  // (the built-in generator needs no DLL: it can be turned on any time)
        n = 4 + kMaxExtra;
        NVN_LOG("lsfg: sin Lossless.dll, generador propio disponible: swapchain de %u imágenes\n", n);
    }
    return caps.maxImageCount ? std::min(n, caps.maxImageCount) : n;
}

// Un error de Vulkan o de la DLL: sin LSFG hasta reiniciar el juego. Lo que hubiera en vuelo se queda (no se
// sabe si la GPU lo sigue leyendo); el juego sigue con el camino de siempre.
void FrameGen::disable(const std::string& why) {
    if (state_ == State::Failed) return;
    state_ = State::Failed;
    NVN_LOG("lsfg: desactivado en esta sesión: %s\n", why.c_str());
}

bool FrameGen::setup() {
    if (set_up_) return true;
    VkContext* c = vk();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = c->queue_family;
    if (VkResult r = vkCreateCommandPool(c->device, &pci, nullptr, &pool_); r != VK_SUCCESS)
        return disable(std::string("vkCreateCommandPool: ") + vk_result_name(r)), false;
    for (Slot& s : slots_) {
        VkCommandBuffer cmds[2];
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(c->device, &ai, cmds) != VK_SUCCESS ||
            vkCreateSemaphore(c->device, &si, nullptr, &s.acquired) != VK_SUCCESS ||
            vkCreateSemaphore(c->device, &si, nullptr, &s.rendered) != VK_SUCCESS ||
            vkCreateFence(c->device, &fi, nullptr, &s.done) != VK_SUCCESS)
            return disable("sin memoria para los envíos"), false;
        s.cmd = cmds[0];
        s.blit = cmds[1];
        for (Extra& e : s.extra) {
            VkCommandBuffer more[2];
            if (vkAllocateCommandBuffers(c->device, &ai, more) != VK_SUCCESS ||
                vkCreateSemaphore(c->device, &si, nullptr, &e.acquired) != VK_SUCCESS ||
                vkCreateSemaphore(c->device, &si, nullptr, &e.rendered) != VK_SUCCESS ||
                vkCreateFence(c->device, &fi, nullptr, &e.done) != VK_SUCCESS)
                return disable("sin memoria para los envíos"), false;
            e.cmd = more[0];
            e.blit = more[1];
        }
    }
    // Tiempo de GPU de LSFG: dos timestamps por envío.
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(c->gpu, &count, families.data());
    const uint32_t bits = c->queue_family < count ? families[c->queue_family].timestampValidBits : 0;
    if (bits) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kSlots;
        if (vkCreateQueryPool(c->device, &qi, nullptr, &queries_) != VK_SUCCESS) queries_ = VK_NULL_HANDLE;
        timestamp_mask_ = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    }
    if (!queries_) NVN_LOG("lsfg: sin timestamps: no se medirá su tiempo de GPU\n");
    set_up_ = true;
    return true;
}

// Un envío que ya acabó: su tiempo de GPU a las cuentas y el hueco libre.
void FrameGen::collect(int index) {
    Slot& s = slots_[index];
    if (s.timed && queries_) {
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(vk()->device, queries_, 2 * index, 2, sizeof ts, ts, sizeof ts[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double gpu = static_cast<double>((ts[1] - ts[0]) & timestamp_mask_) *
                               vk()->props.limits.timestampPeriod / 1e6;
            gpu_sum_ += gpu;
            gpu_max_ = std::max(gpu_max_, gpu);
            gpu_n_++;
            const int64_t ns = static_cast<int64_t>(gpu * 1e6), old = cost_ns_.load(std::memory_order_relaxed);
            cost_ns_.store(old ? old + (ns - old) / 8 : ns, std::memory_order_relaxed);  // media móvil
        }
    }
    s.pending = s.timed = false;
    s.chain = nullptr;
}

void FrameGen::poll_slots() {
    for (Slot& s : slots_)
        for (Extra& e : s.extra)
            if (e.pending && vkGetFenceStatus(vk()->device, e.done) == VK_SUCCESS) e.pending = false;
    for (int i = 0; i < kSlots; i++) {
        if (!slots_[i].pending) continue;
        VkResult r = vkGetFenceStatus(vk()->device, slots_[i].done);
        if (r == VK_SUCCESS) collect(i);
        else if (r != VK_NOT_READY) return disable(std::string("vkGetFenceStatus: ") + vk_result_name(r));
    }
}

// La cadena deja de usarse; se destruye en cuanto la GPU acabe lo que la lee (espera acotada aquí y, si no
// basta, se vuelve a mirar en cada frame sin bloquear).
void FrameGen::retire() {
    if (retired_) reap(true);
    if (retired_) return;  // la anterior sigue en la GPU: esta se queda puesta (sin usarse) hasta el frame siguiente
    retired_ = chain_;
    chain_ = nullptr;
    warm_ = 0;
    reap(true);
}

void FrameGen::reap(bool wait) {
    if (!retired_) return;
    VkFence fences[kSlots * (1 + kMaxExtra)];
    uint32_t n = 0;
    for (Slot& s : slots_)
        if (s.pending && s.chain == retired_) {
            fences[n++] = s.done;
            for (Extra& e : s.extra)
                if (e.pending) fences[n++] = e.done;
        }
    if (n) {
        VkResult r = vkWaitForFences(vk()->device, n, fences, VK_TRUE, wait ? kRetireWaitNs : 0);
        if (r == VK_TIMEOUT) return;
        if (r != VK_SUCCESS) return disable(std::string("vkWaitForFences: ") + vk_result_name(r));
        for (int i = 0; i < kSlots; i++)
            if (slots_[i].pending && slots_[i].chain == retired_) {
                collect(i);
                for (Extra& e : slots_[i].extra) e.pending = false;
            }
    }
    delete retired_;
    retired_ = nullptr;
    NVN_LOG("lsfg: cadena liberada\n");
}

void FrameGen::start_job(VkExtent2D extent, VkFormat format, float flow) {
    {
        std::lock_guard lock(job_.m);
        if (job_.busy) return;
        job_.busy = true;
        job_.done = false;
    }
    const bool builtin = want_builtin();
    const bool need_compile = !compiled_ && !builtin;
    state_ = State::Waiting;
    if (need_compile) NVN_LOG("lsfg: compilando los shaders en segundo plano (hasta entonces, sin generar)\n");
    std::thread([this, extent, format, flow, need_compile, builtin] {
        pthread_setname_np(pthread_self(), "NvnLsfg");
        const Clock::time_point t0 = Clock::now();
        std::string why;
        bool fatal = false, missing = false, fell_back = false;
        std::unique_ptr<Generator> chain;
        if (!builtin) {
            if (need_compile && !compile(&why, &missing)) {
                fatal = !missing;
            } else {
                auto lsfg = std::make_unique<Chain>();
                if (lsfg->create(pipelines_, extent, format, flow, &why)) chain = std::move(lsfg);
                else fatal = true;
            }
        }
        // Without LSFG (none chosen, no DLL, or it can't run here): the built-in one, on any Vulkan 1.1 GPU
        if (!chain && Motion::available()) {
            auto own = std::make_unique<Motion>();
            std::string reason;
            if (own->create(extent, format, flow, &reason)) {
                if (!builtin) NVN_LOG("lsfg: LSFG no va (%s): uso el generador propio\n", why.c_str());
                chain = std::move(own);
                fell_back = !builtin;
                fatal = missing = false;
            } else {
                why += (why.empty() ? "" : "; ") + std::string("generador propio: ") + reason;
            }
        }
        std::lock_guard lock(job_.m);
        job_.fatal = fatal;
        job_.missing = missing;
        job_.fell_back = fell_back;
        job_.chain = chain.release();
        job_.why = why;
        job_.ms = ms_between(t0, Clock::now());
        job_.done = true;
    }).detach();  // vive lo que tarde en compilar; `this` es global
}

void FrameGen::poll_job() {
    std::unique_ptr<Generator> chain;
    std::string why;
    bool fatal, missing, fell_back;
    double took;
    {
        std::lock_guard lock(job_.m);
        if (!job_.done) return;
        job_.done = job_.busy = false;
        chain.reset(job_.chain);
        job_.chain = nullptr;
        why = job_.why;
        fatal = job_.fatal;
        missing = job_.missing;
        fell_back = job_.fell_back;
        took = job_.ms;
    }
    if (fell_back) lsfg_unusable_ = true;  // (no more tries with LSFG this session)
    if (missing) {  // sin DLL: se reintenta al apagar y volver a encender
        NVN_LOG("lsfg: %s; se reintenta al quitar y volver a poner <datos>/lsfg\n", why.c_str());
        missing_dll_ = true;
        state_ = State::Off;
        return;
    }
    if (fatal) return disable(why);
    if (!chain) return disable("no se pudo montar la cadena: " + why);
    NVN_LOG("lsfg: cadena %s %ux%u (formato %d), flujo %.2f, %s: %zu imágenes, %.1f MB, %u descriptor sets; %.0f ms\n",
            chain->name(), chain->extent().width, chain->extent().height, chain->format(), chain->flow(),
            chain->name()[0] == 'L' ? variant_ : "Vulkan 1.1", chain->images(),
            chain->bytes() / 1048576.0, chain->sets(), took);
    chain_ = chain.release();
    state_ = State::Ready;
    warm_ = 0;
}

// En el hilo de fondo: la DLL, las auditorías y los 25 pipelines. FP16 si el dispositivo tiene shaderFloat16; si
// el driver no lo acepta, FP32. Una marca en <datos> antes de crear cada juego: si el driver tira el proceso
// compilando, la vez siguiente se salta ese juego en vez de caer otra vez.
bool FrameGen::compile(std::string* why, bool* missing) {
    VkContext* c = vk();
    const std::string path = find_dll();
    if (path.empty()) {
        *missing = true;
        *why = "Lossless.dll not found (import it from the GPU driver screen, or put it in <datos>/lsfg/ or Download/)";
        return false;
    }
    Modules fp16, fp32;
    size_t bytes = 0;
    const Clock::time_point t0 = Clock::now();
    if (DllStatus s = load_shaders(path, fp16, fp32, &bytes); s != DllStatus::Ok) {
        *why = path + ": " + dll_status_name(s);
        return false;
    }
    NVN_LOG("lsfg: %s (%zu KB): %zu shaders FP16 y %zu FP32 en %.0f ms\n", path.c_str(), bytes >> 10, fp16.size(),
            fp32.size(), ms_between(t0, Clock::now()));

    const std::string crumb16 = data_path("lsfg_compilando_fp16"), crumb32 = data_path("lsfg_compilando_fp32");
    auto build = [&](const Modules& modules, bool float16, const std::string& crumb, std::string* reason) {
        if (FILE* f = fopen(crumb.c_str(), "w")) fclose(f);
        const Clock::time_point t1 = Clock::now();
        const bool ok = pipelines_.create(modules, float16, reason);
        unlink(crumb.c_str());
        if (ok)
            NVN_LOG("lsfg: %zu pipelines %s en %.0f ms\n", pipelines_.size(), float16 ? "FP16" : "FP32",
                    ms_between(t1, Clock::now()));
        return ok;
    };
    std::string reason16;
    const bool force32 = NVN_FLAG("lsfg_fp32");
    if (fp16.empty() || !c->shader_float16 || force32) {
        NVN_LOG("lsfg: uso FP32 (%s)\n", force32 ? "lo pide <datos>/lsfg_fp32"
                                         : fp16.empty() ? "a la DLL le faltan shaders FP16"
                                                        : "el dispositivo no tiene shaderFloat16");
    } else if (access(crumb16.c_str(), F_OK) == 0) {
        NVN_LOG("lsfg: la última vez el proceso murió creando los pipelines FP16 (queda %s): uso FP32\n", crumb16.c_str());
    } else if (build(fp16, true, crumb16, &reason16)) {
        compiled_ = true;
        variant_ = "FP16";
        return true;
    } else {
        NVN_LOG("lsfg: FP16 no vale (%s): pruebo FP32\n", reason16.c_str());
    }
    if (fp32.empty()) {
        *why = "a la DLL le faltan shaders FP32";
        return false;
    }
    if (access(crumb32.c_str(), F_OK) == 0) {
        *why = "la última vez el proceso murió creando los pipelines FP32; borra " + crumb32 + " para reintentar";
        return false;
    }
    std::string reason32;
    if (!build(fp32, false, crumb32, &reason32)) {
        *why = "FP32: " + reason32;
        return false;
    }
    compiled_ = true;
    variant_ = "FP32";
    return true;
}

void FrameGen::log_stats(Clock::time_point now) {
    real_++;
    const double secs = ms_between(stats_start_, now) / 1000.0;
    if (secs < 5.0) return;
    if (NVN_FLAG("verbose_log")) {  // DEPURACIÓN
        if (gpu_n_)
            NVN_LOG("lsfg: %.1f fps reales, %u generados; sin generar: %u calentando, %u tirón, "
                    "%u ritmo, %u GPU ocupada, %u sin imagen; GPU %.2f ms de media (máx %.2f)\n",
                    real_ / secs, generated_, warming_, skip_hold_, skip_pace_, skip_busy_, skip_image_,
                    gpu_sum_ / gpu_n_, gpu_max_);
        else
            NVN_LOG("lsfg: %.1f fps reales, %u generados; sin generar: %u calentando, %u tirón, "
                    "%u ritmo, %u GPU ocupada, %u sin imagen\n",
                    real_ / secs, generated_, warming_, skip_hold_, skip_pace_, skip_busy_, skip_image_);
    }
    stats_start_ = now;
    real_ = generated_ = warming_ = skip_hold_ = skip_pace_ = skip_busy_ = skip_image_ = 0;
    gpu_sum_ = gpu_max_ = 0;
    gpu_n_ = 0;
}

bool FrameGen::record_lsfg(int index, const Texture& t, const VkRect2D& crop, uint64_t n, bool generate) {
    Slot& s = slots_[index];
    VkCommandBuffer cmd = s.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    if (VkResult r = vkBeginCommandBuffer(cmd, &bi); r != VK_SUCCESS)
        return disable(std::string("vkBeginCommandBuffer: ") + vk_result_name(r)), false;
    {  // después del frame del juego (en vuelo, misma cola) y de lo que leyeron el LSFG y los blits anteriores
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
    }
    if (queries_) {  // el primero, cuando acaba todo lo anterior (el frame del juego): mide solo lo nuestro
        vkCmdResetQueryPool(cmd, queries_, 2 * index, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 2 * index);
    }
    if (!chain_->initialized()) chain_->record_init(cmd);
    const Image& in = chain_->input(n);
    // Only the window's crop, as window.cpp presents it: in handheld mode MH Rise draws its 1280x720 picture in the corner
    // of a 1920x1080 texture, and the whole texture made every generated frame show the picture at two thirds of the
    // screen, in turn with the real ones at full size (the picture shrinking and growing at 30 Hz).
    if (in.extent.width == crop.extent.width && in.extent.height == crop.extent.height) {
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {crop.offset.x, crop.offset.y, 0};
        region.extent = {in.extent.width, in.extent.height, 1};
        vkCmdCopyImage(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, in.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    } else {  // lsfg_medium: la cadena es más pequeña que el recorte
        VkImageBlit region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = {crop.offset.x, crop.offset.y, 0};
        region.srcOffsets[1] = {crop.offset.x + static_cast<int32_t>(crop.extent.width),
                                crop.offset.y + static_cast<int32_t>(crop.extent.height), 1};
        region.dstOffsets[1] = {static_cast<int32_t>(in.extent.width), static_cast<int32_t>(in.extent.height), 1};
        vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, in.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_LINEAR);
    }
    {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    if (n >= 1) chain_->record_shared(cmd, n);
    if (generate) {
        chain_->record_generate(cmd, n);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};  // la salida, lista para el blit
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
    }
    if (queries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 2 * index + 1);
    if (VkResult r = vkEndCommandBuffer(cmd); r != VK_SUCCESS)
        return disable(std::string("vkEndCommandBuffer: ") + vk_result_name(r)), false;
    return true;
}

// La salida de la cadena (1280x720) escalada a la imagen del swapchain, igual que window.cpp hace con el real.
bool FrameGen::record_blit(VkCommandBuffer blit, const Window& w, uint32_t image) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(blit, 0);
    if (VkResult r = vkBeginCommandBuffer(blit, &bi); r != VK_SUCCESS)
        return disable(std::string("vkBeginCommandBuffer: ") + vk_result_name(r)), false;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = w.images[image];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(blit, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &b);
    const Image& out = chain_->output();
    // Con FSR, como el frame real (window.cpp): si no, el generado salía más blando y la nitidez parpadeaba a 30 Hz.
    // (in the same rectangle as the real frames: the aspect ratio setting)
    VkExtent2D rect;
    VkOffset2D at;
    picture_rect(w.extent, &rect, &at);
    if (rect.width != w.extent.width || rect.height != w.extent.height) {
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(blit, w.images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }
    if (!fsr::record(blit, out.view, out.extent, out.extent, w.images[image], w.format, rect, at)) {
        VkImageBlit region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[1] = {static_cast<int32_t>(out.extent.width), static_cast<int32_t>(out.extent.height), 1};
        region.dstOffsets[0] = {at.x, at.y, 0};
        region.dstOffsets[1] = {at.x + static_cast<int32_t>(rect.width), at.y + static_cast<int32_t>(rect.height), 1};
        vkCmdBlitImage(blit, out.image, VK_IMAGE_LAYOUT_GENERAL, w.images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &region, VK_FILTER_LINEAR);
    }
    hud::record(blit, w.images[image], w.extent, w.format);  // el HUD también en los generados
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(blit, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    if (VkResult r = vkEndCommandBuffer(blit); r != VK_SUCCESS)
        return disable(std::string("vkEndCommandBuffer: ") + vk_result_name(r)), false;
    return true;
}

void FrameGen::present(Window& w, Texture* t) {
    const bool on = NVN_FLAG("lsfg");
    const Clock::time_point now = Clock::now();
    const double dt = last_ == Clock::time_point{} ? 0.0 : ms_between(last_, now);
    last_ = now;
    if (state_ == State::Failed || (!on && !enabled_ && !chain_ && !retired_)) return;  // apagado: casi gratis
    VkContext* c = vk();
    if (!c || !w.swapchain) return;
    // Primero el del ejecutor (lee t->image, que el juego puede rehacer) y luego el de la cola: el mismo orden que
    // el resto (ejecutor -> cola), sin cruces.
    std::lock_guard executor_lock(executor_mutex());
    std::lock_guard lock(vk_queue_mutex());
    poll_slots();
    poll_job();
    reap(false);
    if (state_ == State::Failed) return;

    if (!on) {
        if (enabled_) NVN_LOG("lsfg: apagado\n");
        enabled_ = missing_dll_ = announced_ = false;
        if (chain_) retire();
        return;
    }
    // Más barato (para que quepa en las zonas pesadas): <datos>/lsfg_flujo_25 (flujo óptico a un cuarto) y
    // <datos>/lsfg_medium (el frame generado a 3/4 de resolución, 960x540; el real sigue a la suya).
    // Flujo a un cuarto por defecto: medido en las Ruinas, 3,5 ms por frame generado frente a 6,5 a la mitad, y así cabe
    // con el frame real en zonas abiertas. lsfg_flow_50 / lsfg_flow_100 para comparar.
    const float flow = NVN_FLAG("lsfg_flow_100") ? 1.0f : NVN_FLAG("lsfg_flow_50") ? 0.5f : 0.25f;
    const bool reduced = NVN_FLAG("lsfg_medium");
    if (!enabled_) {
        enabled_ = true;
        stats_start_ = now;
        NVN_LOG("lsfg: encendido: 2x, modo rendimiento, flujo %.2f, SDR, ui_threshold 0.5\n", flow);
    }
    log_stats(now);
    if (missing_dll_ || !setup()) return;

    // La cadena es para la textura de la ventana de ahora y el flujo pedido; si cambia, se monta otra en el hilo
    // de fondo (la primera vez, con los pipelines) y hasta entonces se presenta como siempre.
    if (!t || !t->image || t->vk_format == VK_FORMAT_UNDEFINED || t->desc.width <= 0 || t->desc.height <= 0) {
        warm_ = 0;
        return;
    }
    // The part of the texture that's shown (the window's crop; none: all of it), within the texture
    VkRect2D crop{};
    crop.offset.x = std::clamp(w.crop.offset.x, 0, t->desc.width - 1);
    crop.offset.y = std::clamp(w.crop.offset.y, 0, t->desc.height - 1);
    crop.extent.width = w.crop.extent.width ? std::min<uint32_t>(w.crop.extent.width, t->desc.width - crop.offset.x)
                                            : static_cast<uint32_t>(t->desc.width - crop.offset.x);
    crop.extent.height = w.crop.extent.height ? std::min<uint32_t>(w.crop.extent.height, t->desc.height - crop.offset.y)
                                              : static_cast<uint32_t>(t->desc.height - crop.offset.y);
    const VkExtent2D extent{crop.extent.width * (reduced ? 3 : 4) / 4, crop.extent.height * (reduced ? 3 : 4) / 4};
    const bool own = chain_ && chain_->name()[0] == 'b';  // (the built-in one)
    if (!chain_ || chain_->extent().width != extent.width || chain_->extent().height != extent.height ||
        chain_->format() != t->vk_format || own != want_builtin() || chain_->flow() != flow) {
        if (chain_) retire();
        if (!chain_ && !retired_) start_job(extent, t->vk_format, flow);
        warm_ = 0;
        return;
    }
    if (w.images.size() < 4) {  // la DLL apareció después de crear el swapchain: el segundo present podría esperar
        if (!few_images_logged_)
            NVN_LOG("lsfg: el swapchain tiene %zu imágenes (no había Lossless.dll al arrancar): reinicia el juego\n",
                    w.images.size());
        few_images_logged_ = true;
        return;
    }

    // ¿Toca? Solo con el juego a ~30 fps (intervalo 2) y sin tirones recientes. When real frames run late (the phone
    // below 30) it keeps generating: the picture stays doubled.
    if (dt > 0) ema_ = ema_ > 0 ? ema_ + (dt - ema_) * 0.1 : dt;
    // The pace the presenter keeps (window.cpp: the FPS limit, or the game's own MaxFps): 30 or slower. A 40 or 60 that
    // falls behind is still too fast to double on a 60 Hz screen (and generating there made it flicker on and off,
    // slowing the real frames each time)
    // Adaptive (a target): as many pictures per real frame as reach it, as far as the screen's refreshes between two
    // real frames allow (one picture each) and 4 at most; it has to hold for kTargetFrames before it changes. Without a
    // target, 2x at 30 fps as always.
    const int target = NVN_FLAG("fg_target_120") ? 120 : NVN_FLAG("fg_target_90") ? 90 : NVN_FLAG("fg_target_60") ? 60 : 0;
    const int64_t vblank = refresh_ns(w);
    const double real_ms = std::max(ema_, frame_period_ns() / 1e6);
    const int vblanks = std::max(1, static_cast<int>(real_ms * 1e6 / static_cast<double>(vblank) + 0.25));
    int want = 2;
    if (target) want = std::clamp(static_cast<int>(std::lround(target * real_ms / 1000.0)), 1, std::min(vblanks, 1 + kMaxExtra + 1));
    if (want == multiplier_) {
        wanted_frames_ = 0;
    } else if (want != wanted_) {
        wanted_ = want;
        wanted_frames_ = 1;
    } else if (++wanted_frames_ >= kTargetFrames || !target) {
        if (target) NVN_LOG("lsfg: objetivo %d fps: %dx (%.1f ms por frame real, %d refrescos de %.1f ms)\n", target, want,
                            real_ms, vblanks, vblank / 1e6);
        multiplier_ = want;
        wanted_frames_ = 0;
    }
    const int m = std::min(multiplier_, vblanks);
    // With display timing each picture gets its time: evenly between the real ones, on whole refreshes
    // (Without a target, 16.7 ms as before, whatever the screen: on a 120 Hz one the real frame right after the generated
    // one would show the generated one for a quarter of the time)
    spacing_ns_ = !target ? 16666667
                          : vblank * (vk()->display_timing ? std::max(1, vblanks / std::max(m, 1)) : 1);
    const bool pace_ok = target ? m >= 2 && ema_ >= kTooFastMs * 0.6
                                : w.present_interval == 2 && ema_ >= kTooFastMs && frame_period_ns() >= 32'000'000;
    if (pace_ok == pace_logged_) {
        pace_logged_ = !pace_ok;
        if (!pace_ok)
            NVN_LOG("lsfg: el juego no va a 30 fps (intervalo %d, %.1f ms por frame, ritmo %.1f ms): sin generar\n",
                    w.present_interval, ema_, frame_period_ns() / 1e6);
    }
    if (!pace_ok) {
        skip_pace_++;
        warm_ = 0;
        return;
    }
    if (dt > kHitchMs) hold_until_ = now + std::chrono::seconds(1);
    if (now < hold_until_) {
        skip_hold_++;
        warm_ = 0;
        return;
    }

    const int index = next_;
    Slot& s = slots_[index];
    for (Extra& e : s.extra)  // (its extra pictures, the same 3 real frames ago)
        if (e.pending) {
            if (vkWaitForFences(c->device, 1, &e.done, VK_TRUE, kSlotWaitNs) != VK_SUCCESS) {
                skip_busy_++;
                warm_ = 0;
                return;
            }
            e.pending = false;
        }
    if (s.pending) {  // el envío de hace 3 frames reales
        VkResult r = vkWaitForFences(c->device, 1, &s.done, VK_TRUE, kSlotWaitNs);
        if (r == VK_TIMEOUT) {
            skip_busy_++;
            warm_ = 0;
            return;
        }
        if (r != VK_SUCCESS) return disable(std::string("vkWaitForFences: ") + vk_result_name(r));
        collect(index);
    }
    const uint64_t n = warm_;
    bool generate = false;
    uint32_t image = 0;
    // The first picture's time (1 / m; 0.5 for 2x). LSFG's constants can only change once the GPU is done with the
    // last generation; if that takes too long, this frame goes without.
    const float t0 = 1.0f / static_cast<float>(std::max(m, 2));
    bool timed = true;
    if (n >= kWarmFrames && t0 != chain_time_) {
        if (chain_->name()[0] == 'L' && !wait_generations(kSlotWaitNs)) {
            timed = false;
            skip_busy_++;
        } else if (chain_->set_time(t0)) {
            chain_time_ = t0;
        }
    }
    if (n >= kWarmFrames && timed) {
        VkResult r = vkAcquireNextImageKHR(c->device, w.swapchain, kAcquireNs, s.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) generate = true;
        else if (r == VK_TIMEOUT || r == VK_NOT_READY || r == VK_ERROR_OUT_OF_DATE_KHR) skip_image_++;
        else return disable(std::string("vkAcquireNextImageKHR: ") + vk_result_name(r));
    } else if (n < kWarmFrames) {
        warming_++;
    }
    // Sin imagen se graba igual la copia y lo compartido: la historia sigue llena para el frame siguiente.
    if (!record_lsfg(index, *t, crop, n, generate) || (generate && !record_blit(s.blit, w, image))) return;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
    si[0].commandBufferCount = 1;
    si[0].pCommandBuffers = &s.cmd;
    si[1].waitSemaphoreCount = 1;  // solo el blit espera a la imagen; la copia y el compute no
    si[1].pWaitSemaphores = &s.acquired;
    si[1].pWaitDstStageMask = &wait_stage;
    si[1].commandBufferCount = 1;
    si[1].pCommandBuffers = &s.blit;
    si[1].signalSemaphoreCount = 1;
    si[1].pSignalSemaphores = &s.rendered;
    vkResetFences(c->device, 1, &s.done);
    if (VkResult r = vkQueueSubmit(c->queue, generate ? 2 : 1, si, s.done); r != VK_SUCCESS)
        return disable(std::string("vkQueueSubmit: ") + vk_result_name(r));
    s.pending = true;
    s.timed = queries_ != VK_NULL_HANDLE;
    s.chain = chain_;
    next_ = (next_ + 1) % kSlots;
    warm_++;
    if (!generate) return;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &w.swapchain;
    pi.pImageIndices = &image;
    // presentID impar (2k+1): window.cpp pregunta luego cuándo salió en pantalla (VK_GOOGLE_display_timing).
    VkPresentTimeGOOGLE when{static_cast<uint32_t>(2 * total_generated_ + 1), 0};
    VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
    if (c->display_timing) {
        times.swapchainCount = 1;
        times.pTimes = &when;
        pi.pNext = &times;
    }
    const int64_t queued_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    VkResult r = vkQueuePresentKHR(c->queue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR)
        return disable(std::string("vkQueuePresentKHR: ") + vk_result_name(r));
    generated_++;
    hud::generated_frame();
    last_id_ = when.presentID;
    total_generated_++;
    last_present_ns_ = queued_ns;
    if (!announced_) NVN_LOG("lsfg: generando: un frame intermedio entre cada par de frames reales\n");
    announced_ = true;
    if (m > 2) present_extras(w, s, n, m, queued_ns);
}

// Every submit still reading the generator (the GPU has to be done before LSFG's constants change)
bool FrameGen::wait_generations(uint64_t timeout_ns) {
    VkFence fences[kSlots * (1 + kMaxExtra)];
    uint32_t count = 0;
    for (Slot& s : slots_) {
        if (s.pending && s.chain == chain_) fences[count++] = s.done;
        for (Extra& e : s.extra)
            if (e.pending) fences[count++] = e.done;
    }
    return !count || vkWaitForFences(vk()->device, count, fences, VK_TRUE, timeout_ns) == VK_SUCCESS;
}

// How long the screen shows each picture (VK_GOOGLE_display_timing's refresh cycle; 60 Hz without it)
int64_t FrameGen::refresh_ns(const Window& w) {
    if (w.swapchain != refresh_of_) {
        refresh_of_ = w.swapchain;
        refresh_ns_ = 16666667;
        VkContext* c = vk();
        auto get = c && c->display_timing ? reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
                                                vkGetDeviceProcAddr(c->device, "vkGetRefreshCycleDurationGOOGLE"))
                                          : nullptr;
        VkRefreshCycleDurationGOOGLE d{};
        if (get && get(c->device, w.swapchain, &d) == VK_SUCCESS && d.refreshDuration >= 4000000 &&
            d.refreshDuration <= 50000000)
            refresh_ns_ = static_cast<int64_t>(d.refreshDuration);
        NVN_LOG("lsfg: la pantalla refresca cada %.2f ms (%.0f Hz)\n", refresh_ns_ / 1e6, 1e9 / refresh_ns_);
    }
    return refresh_ns_;
}

// The pictures after the first between the same pair (m - 2 of them), at 2 / m, 3 / m...: the flows (or LSFG's shared
// passes) are kept, so each is only the last part of the generation, its blit and its present, spaced spacing_ns_
// apart. One that can't get a swapchain image (or, with LSFG, waits too long for the one before) ends the run; the
// real frame follows as always.
void FrameGen::present_extras(Window& w, Slot& s, uint64_t n, int m, int64_t first_ns) {
    VkContext* c = vk();
    const bool lsfg = chain_->name()[0] == 'L';
    for (int k = 2; k < m && k - 2 < kMaxExtra; k++) {
        Extra& e = s.extra[k - 2];
        const VkFence before = k == 2 ? s.done : s.extra[k - 3].done;
        if (lsfg && vkWaitForFences(c->device, 1, &before, VK_TRUE, kSlotWaitNs) != VK_SUCCESS) return;
        if (!chain_->set_time(static_cast<float>(k) / static_cast<float>(m))) return;
        chain_time_ = static_cast<float>(k) / static_cast<float>(m);
        uint32_t image = 0;
        VkResult r = vkAcquireNextImageKHR(c->device, w.swapchain, kAcquireNs, e.acquired, VK_NULL_HANDLE, &image);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            skip_image_++;
            return;
        }
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(e.cmd, 0);
        if (vkBeginCommandBuffer(e.cmd, &bi) != VK_SUCCESS) return disable("vkBeginCommandBuffer");
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};  // (after the last picture's blit read the output)
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(e.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        chain_->record_again(e.cmd, n);
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(e.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        if (vkEndCommandBuffer(e.cmd) != VK_SUCCESS) return disable("vkEndCommandBuffer");
        if (!record_blit(e.blit, w, image)) return;
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
        si[0].commandBufferCount = 1;
        si[0].pCommandBuffers = &e.cmd;
        si[1].waitSemaphoreCount = 1;
        si[1].pWaitSemaphores = &e.acquired;
        si[1].pWaitDstStageMask = &wait_stage;
        si[1].commandBufferCount = 1;
        si[1].pCommandBuffers = &e.blit;
        si[1].signalSemaphoreCount = 1;
        si[1].pSignalSemaphores = &e.rendered;
        vkResetFences(c->device, 1, &e.done);
        if (VkResult q = vkQueueSubmit(c->queue, 2, si, e.done); q != VK_SUCCESS)
            return disable(std::string("vkQueueSubmit: ") + vk_result_name(q));
        e.pending = true;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &e.rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &w.swapchain;
        pi.pImageIndices = &image;
        VkPresentTimeGOOGLE when{static_cast<uint32_t>(2 * total_generated_ + 1),
                                 static_cast<uint64_t>(first_ns + (k - 1) * spacing_ns_)};
        VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
        if (c->display_timing) {
            times.swapchainCount = 1;
            times.pTimes = &when;
            pi.pNext = &times;
        }
        r = vkQueuePresentKHR(c->queue, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR)
            return disable(std::string("vkQueuePresentKHR: ") + vk_result_name(r));
        generated_++;
        hud::generated_frame();
        last_id_ = when.presentID;
        total_generated_++;
        last_present_ns_ = first_ns + (k - 1) * spacing_ns_;  // (the real one goes spacing_ns_ after this)
    }
}

}  // namespace

uint32_t swapchain_images(const VkSurfaceCapabilitiesKHR& caps) { return g_framegen.swapchain_images(caps); }

int64_t reserved_ns() { return g_framegen.reserved_ns(); }

int64_t spacing_ns() { return g_framegen.spacing_ns(); }

int64_t present_generated(Window& w, Texture* t, uint32_t* id) {
    const uint64_t before = g_framegen.generated();
    g_framegen.present(w, t);
    if (g_framegen.generated() == before) return 0;
    if (id) *id = g_framegen.last_id();
    return g_framegen.last_present_ns();
}

}  // namespace nvn::lsfg
