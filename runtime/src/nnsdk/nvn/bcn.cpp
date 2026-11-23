// Descompresión de BC1-BC7 en la CPU (bcn.h). Escrita a partir de la especificación de los formatos (la de D3D, la misma
// que siguen Vulkan y las GPU); las tablas de particiones de BC6H y BC7 están en bcn_tables.h.

#include "nnsdk/nvn/bcn.h"

#include <algorithm>
#include <cstring>

#include "nnsdk/nvn/bcn_tables.h"

namespace nvn::bcn {

Kind kind_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return kBC1;
        case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: return kBC2;
        case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: return kBC3;
        case VK_FORMAT_BC4_UNORM_BLOCK: return kBC4U;
        case VK_FORMAT_BC4_SNORM_BLOCK: return kBC4S;
        case VK_FORMAT_BC5_UNORM_BLOCK: return kBC5U;
        case VK_FORMAT_BC5_SNORM_BLOCK: return kBC5S;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: return kBC6HU;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return kBC6HS;
        case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return kBC7;
        default: return kNone;
    }
}

VkFormat decoded_format(VkFormat f) {
    const bool srgb = f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC2_SRGB_BLOCK ||
                      f == VK_FORMAT_BC3_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK;
    switch (kind_of(f)) {
        case kBC1: case kBC2: case kBC3: case kBC7: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        case kBC4U: return VK_FORMAT_R8_UNORM;
        case kBC4S: return VK_FORMAT_R8_SNORM;
        case kBC5U: return VK_FORMAT_R8G8_UNORM;
        case kBC5S: return VK_FORMAT_R8G8_SNORM;
        // Unsigned BC6H in the packed 11/11/10 float format: half the size of RGBA16F, the same 5-bit exponent, and the
        // mantissa cut to 6 and 5 bits (invisible on skies and lighting). The signed one needs a sign: RGBA16F.
        case kBC6HU: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case kBC6HS: return VK_FORMAT_R16G16B16A16_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

uint32_t texel_bytes(Kind k) {
    switch (k) {
        case kBC4U: case kBC4S: return 1;
        case kBC5U: case kBC5S: return 2;
        case kBC6HU: return 4;
        case kBC6HS: return 8;
        case kNone: return 0;
        default: return 4;
    }
}

uint32_t block_bytes(Kind k) { return k == kBC1 || k == kBC4U || k == kBC4S ? 8 : 16; }

namespace {

// ---- BC1-BC5 ----------------------------------------------------------------------------------------------------------

void rgb565(uint16_t c, int* rgb) {
    const int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2), rgb[1] = (g << 2) | (g >> 4), rgb[2] = (b << 3) | (b >> 2);
}

// El color de BC1 (y de BC2 / BC3, que siempre lo usan de 4 colores): 16 texels RGBA
void color_block(const uint8_t* b, uint8_t out[16][4], bool bc1) {
    const uint16_t c0 = static_cast<uint16_t>(b[0] | b[1] << 8), c1 = static_cast<uint16_t>(b[2] | b[3] << 8);
    int p[4][4];
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = 255;
    p[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || !bc1) {
            p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
            p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
        } else {
            p[2][k] = (p[0][k] + p[1][k]) / 2;
            p[3][k] = 0;
        }
    }
    if (bc1 && c0 <= c1) p[3][3] = 0;  // (el cuarto color de BC1 con c0 <= c1: transparente)
    const uint32_t idx = static_cast<uint32_t>(b[4] | b[5] << 8 | b[6] << 16) | static_cast<uint32_t>(b[7]) << 24;
    for (int i = 0; i < 16; i++) {
        const int* c = p[idx >> (2 * i) & 3];
        for (int k = 0; k < 4; k++) out[i][k] = static_cast<uint8_t>(c[k]);
    }
}

// Un canal de BC4 (y el alfa de BC3, los dos de BC5): 16 valores; con signo, de -127 a 127 (guardados como su byte)
void channel_block(const uint8_t* b, uint8_t out[16], bool sign) {
    int e0 = sign ? static_cast<int8_t>(b[0]) : b[0], e1 = sign ? static_cast<int8_t>(b[1]) : b[1];
    if (sign) e0 = std::max(e0, -127), e1 = std::max(e1, -127);
    int v[8] = {e0, e1};
    if (e0 > e1) {
        for (int i = 1; i < 7; i++) v[i + 1] = ((7 - i) * e0 + i * e1) / 7;
    } else {
        for (int i = 1; i < 5; i++) v[i + 1] = ((5 - i) * e0 + i * e1) / 5;
        v[6] = sign ? -127 : 0;
        v[7] = sign ? 127 : 255;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = static_cast<uint8_t>(v[idx >> (3 * i) & 7]);
}

// ---- lectura de bits, del menos significativo al más ------------------------------------------------------------------

struct Bits {
    uint64_t lo, hi;
    int at = 0;
    explicit Bits(const uint8_t* b) {
        memcpy(&lo, b, 8);
        memcpy(&hi, b + 8, 8);
    }
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, at++) v |= static_cast<uint32_t>((at < 64 ? lo >> at : hi >> (at - 64)) & 1) << i;
        return v;
    }
};

const uint8_t kWeights2[4] = {0, 21, 43, 64};
const uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
const uint8_t* weights(int bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }

// ---- BC7 --------------------------------------------------------------------------------------------------------------

struct Bc7Mode {
    uint8_t subsets, partition_bits, rotation_bits, selector_bits, color_bits, alpha_bits, endpoint_pbits, shared_pbits,
        index_bits, index2_bits;
};
const Bc7Mode kBc7[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

int expand(int v, int bits) { return bits >= 8 ? v : (v << (8 - bits)) | (v >> (2 * bits - 8)); }

void bc7_block(const uint8_t* b, uint8_t out[16][4]) {
    int mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1)) mode++;
    if (mode == 8) {  // (un modo que no existe: negro transparente, como la GPU)
        memset(out, 0, 64);
        return;
    }
    const Bc7Mode& m = kBc7[mode];
    Bits bits(b);
    bits.get(mode + 1);
    const int partition = static_cast<int>(bits.get(m.partition_bits));
    const int rotation = static_cast<int>(bits.get(m.rotation_bits));
    const int selector = static_cast<int>(bits.get(m.selector_bits));
    int ep[6][4] = {};  // los extremos: subconjunto s, e0 = 2s, e1 = 2s + 1
    const int n = m.subsets * 2;
    for (int c = 0; c < 3; c++)
        for (int e = 0; e < n; e++) ep[e][c] = static_cast<int>(bits.get(m.color_bits));
    for (int e = 0; e < n; e++) ep[e][3] = m.alpha_bits ? static_cast<int>(bits.get(m.alpha_bits)) : 255;
    int color_bits = m.color_bits, alpha_bits = m.alpha_bits;
    if (m.endpoint_pbits) {
        for (int e = 0; e < n; e++) {
            const int p = static_cast<int>(bits.get(1));
            for (int c = 0; c < 4; c++) ep[e][c] = ep[e][c] << 1 | p;
        }
        color_bits++;
        if (alpha_bits) alpha_bits++;
    } else if (m.shared_pbits) {
        for (int s = 0; s < m.subsets; s++) {
            const int p = static_cast<int>(bits.get(1));
            for (int e = 2 * s; e < 2 * s + 2; e++)
                for (int c = 0; c < 4; c++) ep[e][c] = ep[e][c] << 1 | p;
        }
        color_bits++;
        if (alpha_bits) alpha_bits++;
    }
    for (int e = 0; e < n; e++) {
        for (int c = 0; c < 3; c++) ep[e][c] = expand(ep[e][c], color_bits);
        ep[e][3] = alpha_bits ? expand(ep[e][3], alpha_bits) : 255;
    }
    const uint8_t* subset = m.subsets == 2 ? kPartition2[partition] : m.subsets == 3 ? kPartition3[partition] : nullptr;
    auto anchor = [&](int i) {
        if (i == 0) return true;
        if (m.subsets == 2) return i == kAnchor2[partition];
        if (m.subsets == 3) return i == kAnchor3a[partition] || i == kAnchor3b[partition];
        return false;
    };
    int index[16], index2[16] = {};
    for (int i = 0; i < 16; i++) index[i] = static_cast<int>(bits.get(m.index_bits - (anchor(i) ? 1 : 0)));
    if (m.index2_bits)
        for (int i = 0; i < 16; i++) index2[i] = static_cast<int>(bits.get(m.index2_bits - (i == 0 ? 1 : 0)));
    for (int i = 0; i < 16; i++) {
        const int s = subset ? subset[i] : 0;
        const int* e0 = ep[2 * s];
        const int* e1 = ep[2 * s + 1];
        int ci = index[i], cb = m.index_bits, ai = index[i], ab = m.index_bits;
        if (m.index2_bits) {  // (modos 4 y 5: el color y el alfa con índices propios; el selector los cambia)
            ai = index2[i], ab = m.index2_bits;
            if (selector) std::swap(ci, ai), std::swap(cb, ab);
        }
        const uint8_t wc = weights(cb)[ci], wa = weights(ab)[ai];
        int px[4];
        for (int c = 0; c < 3; c++) px[c] = ((64 - wc) * e0[c] + wc * e1[c] + 32) >> 6;
        px[3] = ((64 - wa) * e0[3] + wa * e1[3] + 32) >> 6;
        if (rotation) std::swap(px[3], px[rotation - 1]);
        for (int c = 0; c < 4; c++) out[i][c] = static_cast<uint8_t>(px[c]);
    }
}

// ---- BC6H -------------------------------------------------------------------------------------------------------------

// Un campo de la cabecera de un modo: el valor (0 rw, 1 gw, 2 bw, 3 rx, 4 gx, 5 bx, 6 ry, 7 gy, 8 by, 9 rz, 10 gz, 11 bz)
// y sus bits, del bit `lo` al `hi` (con lo > hi, guardados al revés: del más alto al más bajo)
struct Field {
    uint8_t value, hi, lo;
};
enum { RW, GW, BW, RX, GX, BX, RY, GY, BY, RZ, GZ, BZ };
#define F(v, h, l) {v, h, l}
struct Bc6Mode {
    uint8_t code, code_bits, regions, transformed, endpoint_bits, delta[3];
    Field fields[24];
    uint8_t nfields;
};
// La tabla de la especificación, modo a modo, en el orden en que van los bits (después de los del modo)
const Bc6Mode kBc6[14] = {
    {0x00, 2, 2, 1, 10, {5, 5, 5}, {F(GY, 4, 4), F(BY, 4, 4), F(BZ, 4, 4), F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 4, 0),
                                   F(GZ, 4, 4), F(GY, 3, 0), F(GX, 4, 0), F(BZ, 0, 0), F(GZ, 3, 0), F(BX, 4, 0), F(BZ, 1, 1),
                                   F(BY, 3, 0), F(RY, 4, 0), F(BZ, 2, 2), F(RZ, 4, 0), F(BZ, 3, 3)}, 19},
    {0x01, 2, 2, 1, 7, {6, 6, 6}, {F(GY, 5, 5), F(GZ, 4, 4), F(GZ, 5, 5), F(RW, 6, 0), F(BZ, 0, 0), F(BZ, 1, 1), F(BY, 4, 4),
                                  F(GW, 6, 0), F(BY, 5, 5), F(BZ, 2, 2), F(GY, 4, 4), F(BW, 6, 0), F(BZ, 3, 3), F(BZ, 5, 5),
                                  F(BZ, 4, 4), F(RX, 5, 0), F(GY, 3, 0), F(GX, 5, 0), F(GZ, 3, 0), F(BX, 5, 0), F(BY, 3, 0),
                                  F(RY, 5, 0), F(RZ, 5, 0)}, 23},
    {0x02, 5, 2, 1, 11, {5, 4, 4}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 4, 0), F(RW, 10, 10), F(GY, 3, 0), F(GX, 3, 0),
                                   F(GW, 10, 10), F(BZ, 0, 0), F(GZ, 3, 0), F(BX, 3, 0), F(BW, 10, 10), F(BZ, 1, 1), F(BY, 3, 0),
                                   F(RY, 4, 0), F(BZ, 2, 2), F(RZ, 4, 0), F(BZ, 3, 3)}, 18},
    {0x06, 5, 2, 1, 11, {4, 5, 4}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 3, 0), F(RW, 10, 10), F(GZ, 4, 4), F(GY, 3, 0),
                                   F(GX, 4, 0), F(GW, 10, 10), F(GZ, 3, 0), F(BX, 3, 0), F(BW, 10, 10), F(BZ, 1, 1), F(BY, 3, 0),
                                   F(RY, 3, 0), F(BZ, 0, 0), F(BZ, 2, 2), F(RZ, 3, 0), F(GY, 4, 4), F(BZ, 3, 3)}, 20},
    {0x0A, 5, 2, 1, 11, {4, 4, 5}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 3, 0), F(RW, 10, 10), F(BY, 4, 4), F(GY, 3, 0),
                                   F(GX, 3, 0), F(GW, 10, 10), F(BZ, 0, 0), F(GZ, 3, 0), F(BX, 4, 0), F(BW, 10, 10), F(BY, 3, 0),
                                   F(RY, 3, 0), F(BZ, 1, 1), F(BZ, 2, 2), F(RZ, 3, 0), F(BZ, 4, 4), F(BZ, 3, 3)}, 20},
    {0x0E, 5, 2, 1, 9, {5, 5, 5}, {F(RW, 8, 0), F(BY, 4, 4), F(GW, 8, 0), F(GY, 4, 4), F(BW, 8, 0), F(BZ, 4, 4), F(RX, 4, 0),
                                  F(GZ, 4, 4), F(GY, 3, 0), F(GX, 4, 0), F(BZ, 0, 0), F(GZ, 3, 0), F(BX, 4, 0), F(BZ, 1, 1),
                                  F(BY, 3, 0), F(RY, 4, 0), F(BZ, 2, 2), F(RZ, 4, 0), F(BZ, 3, 3)}, 19},
    {0x12, 5, 2, 1, 8, {6, 5, 5}, {F(RW, 7, 0), F(GZ, 4, 4), F(BY, 4, 4), F(GW, 7, 0), F(BZ, 2, 2), F(GY, 4, 4), F(BW, 7, 0),
                                  F(BZ, 3, 3), F(BZ, 4, 4), F(RX, 5, 0), F(GY, 3, 0), F(GX, 4, 0), F(BZ, 0, 0), F(GZ, 3, 0),
                                  F(BX, 4, 0), F(BZ, 1, 1), F(BY, 3, 0), F(RY, 5, 0), F(RZ, 5, 0)}, 19},
    {0x16, 5, 2, 1, 8, {5, 6, 5}, {F(RW, 7, 0), F(BZ, 0, 0), F(BY, 4, 4), F(GW, 7, 0), F(GY, 5, 5), F(GY, 4, 4), F(BW, 7, 0),
                                  F(GZ, 5, 5), F(BZ, 4, 4), F(RX, 4, 0), F(GZ, 4, 4), F(GY, 3, 0), F(GX, 5, 0), F(GZ, 3, 0),
                                  F(BX, 4, 0), F(BZ, 1, 1), F(BY, 3, 0), F(RY, 4, 0), F(BZ, 2, 2), F(RZ, 4, 0), F(BZ, 3, 3)}, 21},
    {0x1A, 5, 2, 1, 8, {5, 5, 6}, {F(RW, 7, 0), F(BZ, 1, 1), F(BY, 4, 4), F(GW, 7, 0), F(BY, 5, 5), F(GY, 4, 4), F(BW, 7, 0),
                                  F(BZ, 5, 5), F(BZ, 4, 4), F(RX, 4, 0), F(GZ, 4, 4), F(GY, 3, 0), F(GX, 4, 0), F(BZ, 0, 0),
                                  F(GZ, 3, 0), F(BX, 5, 0), F(BY, 3, 0), F(RY, 4, 0), F(BZ, 2, 2), F(RZ, 4, 0), F(BZ, 3, 3)}, 21},
    {0x1E, 5, 2, 0, 6, {6, 6, 6}, {F(RW, 5, 0), F(GZ, 4, 4), F(BZ, 0, 0), F(BZ, 1, 1), F(BY, 4, 4), F(GW, 5, 0), F(GY, 5, 5),
                                  F(BY, 5, 5), F(BZ, 2, 2), F(GY, 4, 4), F(BW, 5, 0), F(GZ, 5, 5), F(BZ, 3, 3), F(BZ, 5, 5),
                                  F(BZ, 4, 4), F(RX, 5, 0), F(GY, 3, 0), F(GX, 5, 0), F(GZ, 3, 0), F(BX, 5, 0), F(BY, 3, 0),
                                  F(RY, 5, 0), F(RZ, 5, 0)}, 23},
    {0x03, 5, 1, 0, 10, {10, 10, 10}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 9, 0), F(GX, 9, 0), F(BX, 9, 0)}, 6},
    {0x07, 5, 1, 1, 11, {9, 9, 9}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 8, 0), F(RW, 10, 10), F(GX, 8, 0), F(GW, 10, 10),
                                   F(BX, 8, 0), F(BW, 10, 10)}, 9},
    {0x0B, 5, 1, 1, 12, {8, 8, 8}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 7, 0), F(RW, 10, 11), F(GX, 7, 0), F(GW, 10, 11),
                                   F(BX, 7, 0), F(BW, 10, 11)}, 9},
    {0x0F, 5, 1, 1, 16, {4, 4, 4}, {F(RW, 9, 0), F(GW, 9, 0), F(BW, 9, 0), F(RX, 3, 0), F(RW, 10, 15), F(GX, 3, 0), F(GW, 10, 15),
                                   F(BX, 3, 0), F(BW, 10, 15)}, 9},
};
#undef F

int sign_extend(int v, int bits) { return v & (1 << (bits - 1)) ? v - (1 << bits) : v; }

int unquantize(int v, int bits, bool sign) {
    if (!sign) {
        if (bits >= 15 || v == 0) return v;
        if (v == (1 << bits) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> bits;
    }
    if (bits >= 16) return v;
    bool negative = v < 0;
    if (negative) v = -v;
    int u;
    if (v == 0) u = 0;
    else if (v >= (1 << (bits - 1)) - 1) u = 0x7FFF;
    else u = ((v << 15) + 0x4000) >> (bits - 1);
    return negative ? -u : u;
}

uint16_t finish(int v, bool sign) {  // el valor interpolado, como half
    if (!sign) return static_cast<uint16_t>((v * 31) >> 6);
    if (v < 0) return static_cast<uint16_t>(0x8000 | (((-v) * 31) >> 5));
    return static_cast<uint16_t>((v * 31) >> 5);
}

void bc6h_block(const uint8_t* b, uint16_t out[16][4], bool sign) {
    Bits bits(b);
    const Bc6Mode* m = nullptr;
    const uint32_t two = b[0] & 3;
    if (two < 2) {
        m = &kBc6[two];
        bits.get(2);
    } else {
        const uint32_t five = b[0] & 31;
        for (const Bc6Mode& mode : kBc6)
            if (mode.code_bits == 5 && mode.code == five) m = &mode;
        bits.get(5);
    }
    if (!m) {  // (modos reservados: negro)
        for (int i = 0; i < 16; i++) out[i][0] = out[i][1] = out[i][2] = 0, out[i][3] = 0x3C00;
        return;
    }
    int v[12] = {};
    for (int f = 0; f < m->nfields; f++) {
        const Field& fl = m->fields[f];
        if (fl.hi >= fl.lo) {
            const int n = fl.hi - fl.lo + 1;
            v[fl.value] |= static_cast<int>(bits.get(n)) << fl.lo;
        } else {  // (al revés: del bit `lo` bajando hasta `hi`, uno a uno)
            for (int k = fl.lo; k >= fl.hi; k--) v[fl.value] |= static_cast<int>(bits.get(1)) << k;
        }
    }
    const int partition = m->regions == 2 ? static_cast<int>(bits.get(5)) : 0;
    const int eb = m->endpoint_bits;
    int ep[4][3];  // región 0: w, x; región 1: y, z
    for (int c = 0; c < 3; c++) {
        int w = v[RW + c], x = v[RX + c], y = v[RY + c], z = v[RZ + c];
        if (sign) w = sign_extend(w, eb);
        if (m->transformed) {
            const int d = m->delta[c], mask = (1 << eb) - 1;
            x = (w + sign_extend(x, d)) & mask;
            y = (w + sign_extend(y, d)) & mask;
            z = (w + sign_extend(z, d)) & mask;
            if (sign) x = sign_extend(x, eb), y = sign_extend(y, eb), z = sign_extend(z, eb);
        } else if (sign) {
            x = sign_extend(x, eb), y = sign_extend(y, eb), z = sign_extend(z, eb);
        }
        ep[0][c] = unquantize(w, eb, sign);
        ep[1][c] = unquantize(x, eb, sign);
        ep[2][c] = unquantize(y, eb, sign);
        ep[3][c] = unquantize(z, eb, sign);
    }
    const int index_bits = m->regions == 2 ? 3 : 4;
    const uint8_t* wt = weights(index_bits);
    for (int i = 0; i < 16; i++) {
        const int region = m->regions == 2 ? kPartition2[partition][i] : 0;
        const bool anchor = i == 0 || (m->regions == 2 && i == kAnchor2[partition]);
        const int w = wt[bits.get(index_bits - (anchor ? 1 : 0))];
        for (int c = 0; c < 3; c++) {
            const int a = ep[2 * region][c], e = ep[2 * region + 1][c];
            out[i][c] = finish(((64 - w) * a + w * e + 32) >> 6, sign);
        }
        out[i][3] = 0x3C00;  // (1.0: BC6H no tiene alfa)
    }
}

}  // namespace

void decode(Kind kind, const uint8_t* blocks, uint32_t wb, uint32_t hb, uint8_t* out, size_t pitch, uint32_t width,
            uint32_t height) {
    const uint32_t bb = block_bytes(kind), tb = texel_bytes(kind);
    for (uint32_t by = 0; by < hb; by++) {
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t* b = blocks + (size_t{by} * wb + bx) * bb;
            uint8_t rgba[16][4];
            uint16_t half[16][4];
            uint8_t ch[2][16];
            switch (kind) {
                case kBC1: color_block(b, rgba, true); break;
                case kBC2:
                    color_block(b + 8, rgba, false);
                    for (int i = 0; i < 16; i++) {
                        const int a = b[i / 2] >> (4 * (i & 1)) & 15;
                        rgba[i][3] = static_cast<uint8_t>(a * 17);
                    }
                    break;
                case kBC3:
                    color_block(b + 8, rgba, false);
                    channel_block(b, ch[0], false);
                    for (int i = 0; i < 16; i++) rgba[i][3] = ch[0][i];
                    break;
                case kBC4U: case kBC4S: channel_block(b, ch[0], kind == kBC4S); break;
                case kBC5U: case kBC5S:
                    channel_block(b, ch[0], kind == kBC5S);
                    channel_block(b + 8, ch[1], kind == kBC5S);
                    break;
                case kBC6HU: case kBC6HS: bc6h_block(b, half, kind == kBC6HS); break;
                case kBC7: bc7_block(b, rgba); break;
                default: return;
            }
            for (uint32_t y = 0; y < 4; y++) {
                const uint32_t ty = by * 4 + y;
                if (ty >= height) break;
                uint8_t* row = out + ty * pitch;
                for (uint32_t x = 0; x < 4; x++) {
                    const uint32_t tx = bx * 4 + x;
                    if (tx >= width) break;
                    const int i = static_cast<int>(y * 4 + x);
                    uint8_t* t = row + size_t{tx} * tb;
                    switch (kind) {
                        case kBC4U: case kBC4S: t[0] = ch[0][i]; break;
                        case kBC5U: case kBC5S: t[0] = ch[0][i], t[1] = ch[1][i]; break;
                        case kBC6HU: {  // R in bits 0-10, G 11-21, B 22-31; a half's exponent fits as it is
                            auto small = [](uint16_t h, int bits) -> uint32_t {
                                if (h & 0x8000) return 0;  // (no sign in these formats; unsigned BC6H has none anyway)
                                const uint32_t e = h >> 10 & 31, m = h & 1023;
                                return e << bits | m >> (10 - bits);
                            };
                            const uint32_t v = small(half[i][0], 6) | small(half[i][1], 6) << 11 | small(half[i][2], 5) << 22;
                            memcpy(t, &v, 4);
                            break;
                        }
                        case kBC6HS: memcpy(t, half[i], 8); break;
                        default: memcpy(t, rgba[i], 4); break;
                    }
                }
            }
        }
    }
}

}  // namespace nvn::bcn
