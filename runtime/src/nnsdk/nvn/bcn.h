#pragma once
// Texturas BCn en las GPU que no las leen: las Mali (todos los MediaTek y muchos Exynos), las PowerVR y alguna otra no
// tienen BC1-BC7, que es lo que usa casi todo el juego. Ahí la imagen se crea en un formato sin comprimir (RGBA8, R8 / RG8
// para BC4 / BC5, B10G11R11 para BC6H sin signo y RGBA16F con signo) y cada nivel se descomprime en la CPU al subirlo
// (executor.cpp, upload). En las GPU que sí las tienen (Adreno, y las de escritorio) no cambia nada: se suben comprimidas,
// como siempre.

#include <cstddef>
#include <cstdint>

#include "nnsdk/nvn/vk.h"

namespace nvn::bcn {

enum Kind : uint8_t { kNone, kBC1, kBC2, kBC3, kBC4U, kBC4S, kBC5U, kBC5S, kBC6HU, kBC6HS, kBC7 };

Kind kind_of(VkFormat format);
// El formato en que se guarda descomprimida (VK_FORMAT_UNDEFINED si no es BCn)
VkFormat decoded_format(VkFormat format);
// Bytes por texel una vez descomprimida
uint32_t texel_bytes(Kind kind);
// Bytes de un bloque de 4x4 comprimido
uint32_t block_bytes(Kind kind);

// Descomprime filas de bloques (wb bloques por fila, hb filas, seguidos en `blocks`) en `out`, con `pitch` bytes por fila
// de texels. Solo escribe dentro de width x height texels (los bloques del borde pueden pasarse).
void decode(Kind kind, const uint8_t* blocks, uint32_t wb, uint32_t hb, uint8_t* out, size_t pitch, uint32_t width,
            uint32_t height);

}  // namespace nvn::bcn
