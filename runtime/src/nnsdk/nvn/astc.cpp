#include "nnsdk/nvn/astc.h"

#include <pthread.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "astcenc.h"
#include "nnsdk/ini.h"
#include "nnsdk/nvn/flags.h"
#include "nnsdk/nvn/vk.h"

namespace nvn::astc {

namespace {

// Encoder threads per level. The upload waits for the level, so more threads mean a shorter freeze the first time a
// texture loads; four leaves the game's own threads some room on an 8-core phone.
constexpr unsigned kThreads = 4;

// One context per colour profile, made the first time it's needed (each holds about 1 MB of tables).
struct Encoder {
    std::mutex m;
    astcenc_context* ctx = nullptr;
    bool tried = false;
};
Encoder g_encoders[2];  // [srgb]

astcenc_context* context(Encoder& e, bool srgb) {
    if (e.tried) return e.ctx;
    e.tried = true;
    astcenc_config config;
    if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST, 0, &config) !=
            ASTCENC_SUCCESS ||
        astcenc_context_alloc(&config, kThreads, &e.ctx, nullptr) != ASTCENC_SUCCESS) {
        e.ctx = nullptr;
        NVN_LOG("ASTC: the encoder didn't start (%s); BC textures stay RGBA8\n", srgb ? "sRGB" : "linear");
    }
    return e.ctx;
}

// In the app's own files folder by default (/data/data/<app>/files, NIER_INTERNO): no FUSE layer like the shared storage
// the data folder lives in, which costs time on every file opened, and a level is a file. <data>/astc_cache_in_data_folder
// (at start-up): in the data folder, as before (it survives reinstalling and can be copied to another phone).
std::string cache_dir() {
    static const std::string dir = [] {
        const char* app = getenv("NIER_INTERNO");
        const bool own = app && *app && !NVN_FLAG("astc_cache_in_data_folder");
        std::string d = (own ? std::string(app) : nnsdk::ini::data_dir()) + "/astc_cache";
        mkdir(d.c_str(), 0755);
        return d;
    }();
    return dir;
}

std::string cache_file(uint64_t key) {
    char name[32];
    snprintf(name, sizeof name, "/%016llx.astc", static_cast<unsigned long long>(key));
    return cache_dir() + name;
}

// The writer: one thread, lowest priority, so the SD card isn't busy while the game reads its own files (spirv.bin
// once saturated it and the game fell to 1 fps). At most 64 MB waiting; past that, levels just aren't cached this time.
std::mutex g_write_m;
std::condition_variable g_write_cv;
std::deque<std::pair<uint64_t, std::vector<uint8_t>>> g_writes;
size_t g_write_bytes = 0;

void writer() {
    pthread_setname_np(pthread_self(), "ASTCCache");
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    for (;;) {
        std::pair<uint64_t, std::vector<uint8_t>> job;
        {
            std::unique_lock lock(g_write_m);
            g_write_cv.wait(lock, [] { return !g_writes.empty(); });
            job = std::move(g_writes.front());
            g_writes.pop_front();
            g_write_bytes -= job.second.size();
        }
        const std::string path = cache_file(job.first), tmp = path + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            const bool ok = fwrite(job.second.data(), 1, job.second.size(), f) == job.second.size();
            fclose(f);
            if (ok) rename(tmp.c_str(), path.c_str());
            else unlink(tmp.c_str());
        }
    }
}

}  // namespace

bool wanted() {
    static const bool yes = [] {
        if (!NVN_FLAG("bc_astc")) return false;
        VkContext* c = vk();
        if (!c) return false;
        for (VkFormat f : {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(c->gpu, f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
                NVN_LOG("ASTC: this GPU doesn't sample ASTC 4x4; BC textures stay RGBA8\n");
                return false;
            }
        }
        NVN_LOG("ASTC: BC1/BC2/BC3/BC7 textures stored as ASTC 4x4 (cache in %s)\n", cache_dir().c_str());
        return true;
    }();
    return yes;
}

bool encode(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb, uint8_t* out) {
    Encoder& e = g_encoders[srgb ? 1 : 0];
    std::lock_guard lock(e.m);
    astcenc_context* ctx = context(e, srgb);
    if (!ctx || !width || !height) return false;
    void* slices[1] = {const_cast<uint8_t*>(rgba)};
    astcenc_image image{};
    image.dim_x = width;
    image.dim_y = height;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    image.data = slices;
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const size_t size = size_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
    // Small levels on this thread alone; bigger ones split between kThreads (the context divides the blocks).
    const unsigned threads = size_t{width} * height >= 128 * 128 ? kThreads : 1;
    std::vector<astcenc_error> result(threads, ASTCENC_SUCCESS);
    std::vector<std::thread> workers;
    for (unsigned k = 1; k < threads; k++)
        workers.emplace_back([&, k] { result[k] = astcenc_compress_image(ctx, &image, &swizzle, out, size, k); });
    result[0] = astcenc_compress_image(ctx, &image, &swizzle, out, size, 0);
    for (std::thread& w : workers) w.join();
    astcenc_compress_reset(ctx);
    for (astcenc_error r : result)
        if (r != ASTCENC_SUCCESS) {
            static int logged = 0;
            if (logged++ < 4) NVN_LOG("ASTC: encoding a %ux%u level failed (%s)\n", width, height, astcenc_get_error_string(r));
            return false;
        }
    return true;
}

// ---- BC1 -> ASTC 4x4 --------------------------------------------------------------------------------------------------
// Every block is one partition with a 4x4 grid of weights (one per texel), written per the Khronos ASTC specification
// (the Data Format Specification's ASTC chapter). The approach, BC1's line kept as ASTC's endpoints and its indices as
// weights, is BOTWRecompAndroid's (runtime/nvn/bc1_astc.c, MIT); this is written from the spec. The three kinds of
// BC1 block:
// - four colours (c0 > c1): RGB endpoints of 8 bits (CEM 8), weights of 4 levels (0, 21, 43, 64 of 64: BC1's 0, 1/3,
//   2/3 and 1);
// - three colours (c0 <= c1), none transparent: the same with weights of 3 levels (0, 32, 64), stored as trits;
// - three colours with transparent texels: two weight planes, RGB and alpha (CEM 12, RGBA endpoints of 7 bits: what
//   fits). A transparent texel takes the darker endpoint's colour instead of black (only seen when filtered).

namespace {

struct Block {
    uint64_t w[2] = {0, 0};
    void put(unsigned pos, unsigned bits, uint32_t v) {  // bits [pos, pos + bits), lowest first (v has no more)
        const unsigned at = pos & 63;
        w[pos >> 6] |= uint64_t{v} << at;
        if (at + bits > 64) w[(pos >> 6) + 1] |= uint64_t{v} >> (64 - at);
    }
    // The weight data: written from bit 0 here, then turned around into the top of the block (bit 0 -> bit 127)
    void put_reversed(const Block& weights) {
        static const auto rev8 = [] {
            std::array<uint8_t, 256> r{};
            for (int i = 0; i < 256; i++)
                for (int b = 0; b < 8; b++)
                    if (i >> b & 1) r[i] |= static_cast<uint8_t>(0x80 >> b);
            return r;
        }();
        auto rev64 = [](uint64_t x) {
            uint64_t r = 0;
            for (int i = 0; i < 8; i++) r = r << 8 | rev8[x >> (8 * i) & 255];
            return r;
        };
        w[0] |= rev64(weights.w[1]);
        w[1] |= rev64(weights.w[0]);
    }
};

// Five trits from an 8-bit trit block, as the decoder reads them (the specification's integer sequence decoding)
void trits_of(unsigned T, unsigned t[5]) {
    auto bit = [T](int i) { return T >> i & 1; };
    unsigned C;
    if ((T >> 2 & 7) == 7) {
        C = (T >> 5 & 7) << 2 | (T & 3);
        t[4] = t[3] = 2;
    } else {
        C = T & 31;
        if ((T >> 5 & 3) == 3) t[4] = 2, t[3] = bit(7);
        else t[4] = bit(7), t[3] = T >> 5 & 3;
    }
    if ((C & 3) == 3) t[2] = 2, t[1] = C >> 4 & 1, t[0] = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1);
    else if ((C >> 2 & 3) == 3) t[2] = t[1] = 2, t[0] = C & 3;
    else t[2] = C >> 4 & 1, t[1] = C >> 2 & 3, t[0] = (C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1);
}

// The trit block that gives the first `k` trits of v, among those whose bits past what a group of k stores (2, 4, 5, 7
// or 8 bits) are zero: the decoder reads those as zero
unsigned trit_block(const unsigned v[5], int k) {
    // A table per group length, made once: [k][the k trits as a base-3 number]
    static const auto table = [] {
        static const int stored[6] = {0, 2, 4, 5, 7, 8};
        std::vector<std::vector<uint8_t>> t(6);
        for (int len = 1; len <= 5; len++) {
            int n = 1;
            for (int i = 0; i < len; i++) n *= 3;
            t[len].assign(n, 0);
            std::vector<bool> found(n, false);
            for (unsigned T = 0; T < (1u << stored[len]); T++) {
                unsigned d[5];
                trits_of(T, d);
                int key = 0;
                for (int i = len - 1; i >= 0; i--) key = key * 3 + static_cast<int>(d[i]);
                if (!found[key]) found[key] = true, t[len][key] = static_cast<uint8_t>(T);
            }
        }
        return t;
    }();
    int key = 0;
    for (int i = k - 1; i >= 0; i--) key = key * 3 + static_cast<int>(v[i]);
    return table[k][key];
}

// n weights of 3 levels (0, 1, 2), packed as trits: the weight data, read from bit 127 down
void put_trit_weights(Block& b, const unsigned* v, int n) {
    Block weights;
    unsigned at = 0;
    for (int g = 0; g < n; g += 5) {
        const int k = std::min(5, n - g);
        unsigned group[5] = {0, 0, 0, 0, 0};
        for (int i = 0; i < k; i++) group[i] = v[g + i];
        static const int stored[6] = {0, 2, 4, 5, 7, 8};
        weights.put(at, stored[k], trit_block(group, k));
        at += stored[k];
    }
    b.put_reversed(weights);
}

void put_bit_weights(Block& b, const unsigned* v, int n, unsigned bits) {
    Block weights;
    for (int i = 0; i < n; i++) weights.put(i * bits, bits, v[i]);
    b.put_reversed(weights);
}

void rgb565(unsigned c, int out[3]) {
    const int r = c >> 11 & 31, g = c >> 5 & 63, bl = c & 31;
    out[0] = r << 3 | r >> 2, out[1] = g << 2 | g >> 4, out[2] = bl << 3 | bl >> 2;
}

void bc1_block(const uint8_t* in, uint8_t* out) {
    const unsigned c0 = in[0] | in[1] << 8, c1 = in[2] | in[3] << 8;
    const uint32_t idx = static_cast<uint32_t>(in[4] | in[5] << 8 | in[6] << 16) | static_cast<uint32_t>(in[7]) << 24;
    int e[2][3];
    rgb565(c0, e[0]);
    rgb565(c1, e[1]);
    const bool four = c0 > c1;
    bool transparent = false;
    for (int i = 0; i < 16 && !four; i++) transparent = transparent || (idx >> (2 * i) & 3) == 3;
    // With transparent texels the endpoints go in 7 bits, which the decoder widens back as x << 1 | x >> 6
    if (transparent)
        for (auto& c : e)
            for (int& v : c) v = (v >> 1) << 1 | v >> 7;
    // The decoder blue-contracts the endpoints when the second one sums less than the first (as it reads them, after
    // the widening above): order them so it doesn't
    const bool swap = e[1][0] + e[1][1] + e[1][2] < e[0][0] + e[0][1] + e[0][2];
    const int* lo = e[swap ? 1 : 0];
    const int* hi = e[swap ? 0 : 1];
    unsigned wt[16];
    for (int i = 0; i < 16; i++) {
        const unsigned s = idx >> (2 * i) & 3;
        if (four) {
            static const unsigned level[4] = {0, 3, 1, 2};  // BC1 index -> position on the line, 0..3
            wt[i] = swap ? 3 - level[s] : level[s];
        } else {
            static const unsigned level[4] = {0, 2, 1, 0};  // 0..2; the transparent one takes the darker end
            wt[i] = s == 3 ? 0 : swap ? 2 - level[s] : level[s];
        }
    }
    Block b;
    if (four) {
        b.put(0, 11, 0x042);  // 4x4 weights, 4 levels, one plane
        b.put(13, 4, 8);      // CEM 8: RGB, direct
        for (int k = 0; k < 3; k++) b.put(17 + 16 * k, 8, lo[k]), b.put(25 + 16 * k, 8, hi[k]);
        put_bit_weights(b, wt, 16, 2);
    } else if (!transparent) {
        b.put(0, 11, 0x051);  // 4x4 weights, 3 levels, one plane
        b.put(13, 4, 8);
        for (int k = 0; k < 3; k++) b.put(17 + 16 * k, 8, lo[k]), b.put(25 + 16 * k, 8, hi[k]);
        put_trit_weights(b, wt, 16);
    } else {
        b.put(0, 11, 0x451);  // 4x4 weights, 3 levels, two planes
        b.put(13, 4, 12);     // CEM 12: RGBA, direct
        // 8 endpoint values of 7 bits (what's left beside 32 trit weights); the decoder widens x to x << 1 | x >> 6
        const int a[2] = {0, 127};
        for (int k = 0; k < 3; k++) b.put(17 + 14 * k, 7, lo[k] >> 1), b.put(24 + 14 * k, 7, hi[k] >> 1);
        b.put(59, 7, a[0]), b.put(66, 7, a[1]);
        unsigned both[32];  // per texel: the RGB weight, then the alpha one
        for (int i = 0; i < 16; i++) both[2 * i] = wt[i], both[2 * i + 1] = (idx >> (2 * i) & 3) == 3 ? 0 : 2;
        put_trit_weights(b, both, 32);
        b.put(128 - 52 - 2, 2, 3);  // the second plane is alpha (just below the 52 bits of weights)
    }
    memcpy(out, b.w, 16);
}

}  // namespace

void from_bc1(const uint8_t* bc1, uint8_t* out, size_t count) {
    for (size_t i = 0; i < count; i++) bc1_block(bc1 + 8 * i, out + 16 * i);
}

bool load(uint64_t key, size_t size, uint8_t* out) {
    FILE* f = fopen(cache_file(key).c_str(), "rb");
    if (!f) return false;
    const bool ok = fread(out, 1, size, f) == size && fgetc(f) == EOF;
    fclose(f);
    return ok;
}

void store(uint64_t key, const uint8_t* data, size_t size) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(writer).detach(); });  // lives as long as the process
    {
        std::lock_guard lock(g_write_m);
        if (g_write_bytes + size > (64u << 20)) return;
        g_writes.emplace_back(key, std::vector<uint8_t>(data, data + size));
        g_write_bytes += size;
    }
    g_write_cv.notify_one();
}

}  // namespace nvn::astc
