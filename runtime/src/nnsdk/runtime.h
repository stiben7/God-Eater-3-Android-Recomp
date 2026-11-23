#pragma once
// Servicios internos de libnnsdk.so compartidos entre módulos (os, libc…).

#include "nnsdk/host.h"

namespace nnsdk {

const NnsdkHost& host();

// Cada hilo nuevo del juego avisa al loader (pila alternativa para el crash handler).
void on_thread_start();

// Docked mode (oe.cpp): <datos>/dock_mode when the game started. The game thinks it's on the dock (TV output, a
// controller instead of the handheld): its own docked resolution, about twice the pixels.
bool docked();

}  // namespace nnsdk
