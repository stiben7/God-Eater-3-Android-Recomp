#pragma once
// Escalado de la imagen del juego a la pantalla con FSR 1 de AMD (EASU reconstruye de 1280x720 a 1920x1080, RCAS añade
// nitidez), en vez del blit lineal: el mismo coste que un efecto de posproceso pequeño (~1 ms) y bastante más nítido.
// <datos>/no_fsr vuelve al blit de siempre en vivo.

#include "nnsdk/nvn/vk.h"

namespace nvn::fsr {

// Escala src (vista muestreable, en GENERAL, de tamaño src_extent) a dst, una imagen del swapchain en
// TRANSFER_DST_OPTIMAL (dst_extent, formato dst_format). false si no puede (formato, recursos, push descriptors): quien
// llama hace el blit de siempre.
// viewport: la zona de src (desde 0,0) que se escala; src_extent, el tamaño de la textura entera.
// dst_extent and dst_offset: the rectangle of dst the picture goes to (the aspect ratio setting, window.cpp).
bool record(VkCommandBuffer cmd, VkImageView src, VkExtent2D src_extent, VkExtent2D viewport, VkImage dst,
            VkFormat dst_format, VkExtent2D dst_extent, VkOffset2D dst_offset = {});

}  // namespace nvn::fsr
