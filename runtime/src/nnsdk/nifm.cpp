// nn::nifm: no internet for the game, as in airplane mode, except for Local Play. Initialize works, a network request is
// settled at once as refused (never left "on hold", which the game polls) and there's no IP or proxy. With
// SetLocalNetworkMode(true) (Local Play, ldn.cpp, which reaches other players through hub servers or directly) the
// request is accepted: IsNetworkAvailable says yes and the game goes on to nn::ldn.

#include <atomic>
#include <cstdio>

#include "nnsdk/types.h"

struct in_addr;

namespace nn::socket {
struct InAddr;
}

namespace nn::nifm {

struct ProxySetting;

namespace {

// nifm (110) 300: "no hay conexión a Internet", lo que contesta el servicio a quien pide la IP o el proxy sin red.
constexpr Result kResultNoInternetConnection{110 | (300 << 9)};

}  // namespace

Result Initialize() { return ResultSuccess; }

std::atomic<bool> g_local{false};      // SetLocalNetworkMode(true): a Local Play request
std::atomic<bool> g_accepted{false};   // the current request was accepted

void SubmitNetworkRequest() {
    g_accepted = g_local.load();
    static std::atomic<int> logged{0};
    if (logged++ < 4) printf("[nifm] network request: %s\n", g_accepted ? "accepted (Local Play)" : "refused (no connection)");
}
void CancelNetworkRequest() { g_accepted = false; }
void SetLocalNetworkMode(bool local) { g_local = local; }
bool IsNetworkRequestOnHold() { return false; }
bool IsNetworkAvailable() { return g_accepted.load(); }
// En la consola enseña el applet de error de red; aquí no hay nada que el usuario pueda arreglar: false.
bool HandleNetworkRequestErrorResult() { return false; }

// Sin red no se tocan las salidas: el juego las inicializa a -1 y solo las lee si hay éxito.
Result GetCurrentPrimaryIpAddress(in_addr*) { return kResultNoInternetConnection; }
Result GetCurrentPrimaryIpAddress(socket::InAddr*) { return kResultNoInternetConnection; }
Result GetCurrentIpConfigInfo(in_addr*, in_addr*, in_addr*, in_addr*, in_addr*) { return kResultNoInternetConnection; }
Result GetCurrentProxySetting(ProxySetting*) { return kResultNoInternetConnection; }

}  // namespace nn::nifm
