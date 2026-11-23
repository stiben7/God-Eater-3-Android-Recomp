// Driver de Vulkan del sistema o del usuario (driver.h).
#include "nnsdk/nvn/driver.h"

#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "adrenotools/driver.h"
#include "nnsdk/nvn/nvn.h"

namespace nvn::driver {

namespace {

bool g_custom = false;
std::string g_folder;  // la carpeta del driver en uso (vacía: el del sistema)

// <interno>/drivers (android_main pone NIER_INTERNO: el internalDataPath de la actividad)
std::string root() {
    const char* home = getenv("NIER_INTERNO");
    return std::string(home && *home ? home : ".") + "/drivers";
}

std::string first_line(FILE* f) {
    char line[512] = {};
    if (!fgets(line, sizeof line, f)) return {};
    line[strcspn(line, "\r\n")] = 0;
    return line;
}

// Deja el elegido en fallo con el motivo: el siguiente arranque ya no lo intenta y la pantalla de inicio lo cuenta
void drop(const std::string& folder, const char* why) {
    const std::string r = root();
    if (FILE* f = fopen((r + "/fallo").c_str(), "w")) {
        fprintf(f, "%s: %s\n", folder.c_str(), why);
        fclose(f);
    }
    unlink((r + "/elegido").c_str());
    unlink((r + "/probando").c_str());
}

// La carpeta de las librerías de la app (nativeLibraryDir), donde están los hooks de libadrenotools: la de libnnsdk.so
std::string library_dir() {
    Dl_info me{};
    if (!dladdr(reinterpret_cast<void*>(&load), &me) || !me.dli_fname) return {};
    std::string dir = me.dli_fname;
    const size_t slash = dir.rfind('/');
    return slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
}

void* open_custom() {
    const std::string r = root();
    FILE* f = fopen((r + "/elegido").c_str(), "r");
    if (!f) return nullptr;
    const std::string folder = first_line(f), library = first_line(f);
    fclose(f);
    if (folder.empty() || library.empty()) return nullptr;
    if (access((r + "/probando").c_str(), F_OK) == 0) {
        NVN_LOG("driver %s: the previous start never drew a frame; using the system driver\n", folder.c_str());
        drop(folder, "the game never drew a frame with it");
        return nullptr;
    }
    const std::string hooks = library_dir(), dir = r + "/" + folder + "/";
    if (FILE* t = fopen((r + "/probando").c_str(), "w")) fclose(t);
    void* lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks.c_str(), dir.c_str(),
                                           library.c_str(), nullptr, nullptr);
    if (!lib) {
        const char* err = dlerror();
        NVN_LOG("driver %s (%s): libadrenotools could not open it (%s); using the system driver\n", folder.c_str(), library.c_str(),
                err ? err : "no details");
        drop(folder, "could not be opened (not an Adreno, or not an Android driver?)");
        return nullptr;
    }
    g_custom = true;
    g_folder = folder;
    NVN_LOG("custom driver: %s (%s, hooks in %s)\n", folder.c_str(), library.c_str(), hooks.c_str());
    return lib;
}

// dl_iterate_phdr: alguna librería cargada desde la carpeta del driver (por el final de la ruta: el linker guarda
// /data/data/... donde a la app se le dio /data/user/0/...)
int from_folder(dl_phdr_info* info, size_t, void* data) {
    return info->dlpi_name && strstr(info->dlpi_name, static_cast<const char*>(data)) ? 1 : 0;
}

}  // namespace

bool load(bool system) {
    void* lib = system ? nullptr : open_custom();
    if (!lib) {
        g_custom = false, g_folder.clear();
        lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    auto gipa = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")) : nullptr;
    if (!gipa) {
        NVN_LOG("no vkGetInstanceProcAddr (%s)\n", g_custom ? "custom driver" : "system libvulkan.so");
        return false;
    }
    volkInitializeCustom(gipa);
    return true;
}

void load_instance(VkInstance instance) { volkLoadInstance(instance); }

void check() {
    if (!g_custom) return;
    // Con un nombre que el sistema ya tiene cargado (vulkan.adreno.so) o un hook que no se instaló, libadrenotools
    // devuelve una libvulkan válida que abre el driver del sistema sin decir nada: se mira si de verdad corre el suyo.
    const std::string part = "/drivers/" + g_folder + "/";
    if (dl_iterate_phdr(from_folder, const_cast<char*>(part.c_str()))) return;
    NVN_LOG("driver %s: none of its libraries got loaded, the system driver is running\n", g_folder.c_str());
    drop(g_folder, "did not load (the system driver was running)");
    g_custom = false, g_folder.clear();
}

void give_up(const char* why) {
    if (!g_custom) return;
    NVN_LOG("driver %s: %s; retrying with the system driver\n", g_folder.c_str(), why);
    drop(g_folder, why);
    g_custom = false, g_folder.clear();
}

void works() {
    static bool done = false;  // (una vez: se llama en cada present)
    if (done || !g_custom) return;
    done = true;
    if (unlink((root() + "/probando").c_str()) == 0) NVN_LOG("driver %s: draws fine, keeping it\n", g_folder.c_str());
}

bool custom() { return g_custom; }

}  // namespace nvn::driver
