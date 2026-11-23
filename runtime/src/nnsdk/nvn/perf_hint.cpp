// Pistas de rendimiento de Android (ADPF, Android 13 en adelante): los hilos críticos (el principal del juego, MainDraw y
// NvnSubmit, los de os.cpp) van en una sesión cuyo objetivo es lo que dura un frame (window.cpp, frame_period_ns), y en cada frame
// se le dice cuánto trabajó el más cargado. Así el sistema sube los relojes (y elige núcleos) para llegar al frame,
// en vez de bajarlos en cada espera del frame y subirlos tarde, que era lo que hacía el ritmo irregular en teléfonos
// fuera de la Flip: con Turnip en una Adreno 725, la GPU del frame en 1,4 ms y aun así frames de 40-80 ms sueltos.
// El trabajo de un hilo es su tiempo de CPU entre dos frames (lo que calculó; sus esperas no cuentan): la suma de los
// tres contaría dos veces lo que se solapa, el más cargado es el que marca el ritmo.
// Las funciones se buscan en libandroid.so al arrancar (la app empieza en Android 9: sin ellas, nada). La sesión se crea
// en un hilo aparte (son llamadas al sistema por binder; el frame solo informa, y eso el sistema lo agrupa).
// <datos>/no_adpf (en vivo): no se informa (el sistema vuelve solo a lo suyo).
#include <dlfcn.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nnsdk/nvn/commands.h"
#include "nnsdk/nvn/flags.h"
#include "nnsdk/nvn/nvn.h"

namespace nvn {

namespace {

struct APerformanceHintManager;
struct APerformanceHintSession;

struct Adpf {
    APerformanceHintManager* (*get_manager)() = nullptr;
    APerformanceHintSession* (*create_session)(APerformanceHintManager*, const int32_t*, size_t, int64_t) = nullptr;
    int (*update_target)(APerformanceHintSession*, int64_t) = nullptr;
    int (*report)(APerformanceHintSession*, int64_t) = nullptr;
    int (*set_threads)(APerformanceHintSession*, const pid_t*, size_t) = nullptr;  // (Android 14)
    bool ok = false;
};

const Adpf& adpf() {
    static const Adpf api = [] {
        Adpf a;
        void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (lib) {
            a.get_manager = reinterpret_cast<decltype(a.get_manager)>(dlsym(lib, "APerformanceHint_getManager"));
            a.create_session = reinterpret_cast<decltype(a.create_session)>(dlsym(lib, "APerformanceHint_createSession"));
            a.update_target =
                reinterpret_cast<decltype(a.update_target)>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
            a.report = reinterpret_cast<decltype(a.report)>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
            a.set_threads = reinterpret_cast<decltype(a.set_threads)>(dlsym(lib, "APerformanceHint_setThreads"));
        }
        a.ok = a.get_manager && a.create_session && a.update_target && a.report;
        NVN_LOG("performance hints (ADPF): %s\n", a.ok ? "yes" : "no (Android older than 13)");
        return a;
    }();
    return api;
}

std::atomic<APerformanceHintSession*> g_session{nullptr};
std::atomic<bool> g_busy{false};  // hay un hilo creando la sesión o cambiando sus hilos
std::vector<pid_t> g_in_session;  // los de la sesión (solo el hilo que presenta)
int64_t g_target = 0;

// El reloj de CPU de otro hilo del proceso (el de pthread_getcpuclockid, desde su tid)
int64_t thread_cpu_ns(pid_t tid) {
    const clockid_t clock = static_cast<clockid_t>((~static_cast<unsigned>(tid) << 3) | 6);  // CPUCLOCK_SCHED por hilo
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) return -1;
    return int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
}

// Crea la sesión, o le cambia los hilos, desde un hilo propio
void regroup(std::vector<pid_t> tids, int64_t target) {
    g_busy = true;
    std::thread([tids = std::move(tids), target] {
        pthread_setname_np(pthread_self(), "PistasADPF");
        const Adpf& a = adpf();
        if (APerformanceHintSession* s = g_session.load()) {
            if (a.set_threads) a.set_threads(s, tids.data(), tids.size());
        } else if (APerformanceHintManager* m = a.get_manager()) {
            std::vector<int32_t> ids(tids.begin(), tids.end());
            APerformanceHintSession* made = a.create_session(m, ids.data(), ids.size(), target);
            NVN_LOG("performance hints: session %s with %zu threads, target %.1f ms\n", made ? "created" : "refused",
                    ids.size(), target / 1e6);
            g_session = made;
        }
        g_busy = false;
    }).detach();
}

}  // namespace

void perf_hint_frame(int64_t period_ns) {
    static std::unordered_map<pid_t, int64_t> last_cpu;
    static bool given_up = false;
    if (given_up || NVN_FLAG("no_adpf")) return;
    const Adpf& a = adpf();
    if (!a.ok) {
        given_up = true;
        return;
    }
    const int64_t target = std::max<int64_t>(period_ns, 8'000'000);
    std::vector<pid_t> tids = critical_threads();
    std::sort(tids.begin(), tids.end());
    APerformanceHintSession* s = g_session.load();

    // Los hilos: la sesión se crea con los que haya; si cambian, se rehace (sin setThreads, Android 13, se queda con los
    // primeros: los tres críticos viven todo el juego)
    if (!tids.empty() && tids != g_in_session && !g_busy.load() && (!s || a.set_threads)) {
        static int tries = 0;
        if (s || tries++ < 3) {
            g_in_session = tids;
            g_target = target;
            regroup(tids, target);
        }
        return;
    }
    if (!s) return;
    if (target != g_target) {
        g_target = target;
        a.update_target(s, target);
    }

    // El trabajo del frame: el tiempo de CPU del más cargado desde el frame anterior
    int64_t work = 0;
    for (pid_t tid : g_in_session) {
        const int64_t now = thread_cpu_ns(tid);
        if (now < 0) continue;  // el hilo terminó
        auto [it, fresh] = last_cpu.try_emplace(tid, now);
        if (!fresh) work = std::max(work, now - it->second);
        it->second = now;
    }
    if (work > 0) a.report(s, work);
}

// ---- thermal headroom ----------------------------------------------------------------------------------------------
// Android's own forecast of throttling (AThermal_getThermalHeadroom, Android 12): for the overlay's HEAT line. Android asks for no more than one reading every 10 s, so a thread of its own keeps
// the last one; the frame only reads an atomic.

namespace {
struct AThermalManager;
std::atomic<float> g_headroom{-1.0f};
}  // namespace

float thermal_headroom() {
    static const bool started = [] {
        std::thread([] {
            pthread_setname_np(pthread_self(), "Thermal");
            void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
            auto acquire = lib ? reinterpret_cast<AThermalManager* (*)()>(dlsym(lib, "AThermal_acquireManager")) : nullptr;
            auto headroom = lib ? reinterpret_cast<float (*)(AThermalManager*, int)>(dlsym(lib, "AThermal_getThermalHeadroom"))
                                : nullptr;
            AThermalManager* m = acquire && headroom ? acquire() : nullptr;
            NVN_LOG("thermal headroom: %s\n", m ? "yes" : "no (Android older than 12)");
            if (!m) return;
            for (;;) {
                const float h = headroom(m, 0);  // (now, not a forecast: forecasts are less accurate)
                if (h == h) g_headroom.store(h, std::memory_order_relaxed);  // (NaN: too soon, or not on this phone)
                timespec ts{11, 0};
                nanosleep(&ts, nullptr);
            }
        }).detach();
        return true;
    }();
    (void)started;
    return g_headroom.load(std::memory_order_relaxed);
}

}  // namespace nvn
