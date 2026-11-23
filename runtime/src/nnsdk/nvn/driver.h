#pragma once
// El driver de Vulkan: el del sistema, o uno del usuario para una Adreno (Turnip u otro), elegido en la pantalla de inicio
// (SetupActivity, "Driver de GPU") y cargado con libadrenotools. Las vk* son los punteros de volk: se llenan aquí, desde el
// vkGetInstanceProcAddr del driver que toque.
//
// <interno>/drivers/ (files/ de la app; ahí lo deja SetupActivity, el linker no carga librerías de la SD):
//   <carpeta>/<librería>.so   cada driver importado, con su meta.json
//   elegido                   la carpeta y la librería del que se usa (sin él, el del sistema)
//   probando                  se crea al abrir el del usuario y se borra cuando ya ha presentado frames: si sigue ahí al
//                             arrancar, la vez anterior no llegó, y se vuelve al del sistema
//   fallo                     por qué se dejó (lo enseña la pantalla de inicio y lo borra)

#include <volk.h>

namespace nvn::driver {

// Antes de vkCreateInstance. system: el del sistema aunque haya uno elegido (el reintento de vk.cpp). false: sin Vulkan.
bool load(bool system = false);
// Tras vkCreateInstance: las funciones de instancia y de dispositivo, de ese driver
void load_instance(VkInstance instance);
// Con el dispositivo creado: si el driver del usuario es el que corre de verdad (si no, se apunta el fallo)
void check();
// El del usuario no sirvió en este arranque (vk.cpp vuelve al del sistema)
void give_up(const char* why);
// Ya se han presentado frames con él
void works();
bool custom();

}  // namespace nvn::driver
