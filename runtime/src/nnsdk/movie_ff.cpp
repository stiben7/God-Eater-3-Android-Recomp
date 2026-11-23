// Ver movie_ff.h.
#include "nnsdk/movie_ff.h"

#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#ifdef MHRISE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}
#endif

namespace movie_ff {

#ifndef MHRISE_FFMPEG

Demuxer* open(const uint8_t*, size_t) { return nullptr; }
void destroy(Demuxer*) {}
int track_count(Demuxer*) { return 0; }
AMediaFormat* track_format(Demuxer*, int) { return nullptr; }
bool select_track(Demuxer*, int, bool) { return false; }
int next_track(Demuxer*) { return -1; }
int64_t sample_time(Demuxer*) { return -1; }
SwVideo* sw_open(const uint8_t*, size_t, const uint8_t*, size_t) { return nullptr; }
void sw_close(SwVideo*) {}
void sw_flush(SwVideo*) {}
void sw_decode(SwVideo*, const uint8_t*, size_t, int64_t, std::vector<SwFrame>*) {}
bool sample_sync(Demuxer*) { return false; }
ssize_t sample_size(Demuxer*) { return -1; }
ssize_t read_sample(Demuxer*, uint8_t*, size_t) { return -1; }
bool advance(Demuxer*) { return false; }
bool seek(Demuxer*, int64_t, bool) { return false; }
int64_t duration_us(Demuxer*) { return 0; }

#else

namespace {
constexpr int kMaxTracks = 16;
const uint8_t kStart[4] = {0, 0, 0, 1};

struct MemIo {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

int io_read(void* opaque, uint8_t* buf, int len) {
    auto* m = static_cast<MemIo*>(opaque);
    if (m->pos >= m->size) return AVERROR_EOF;
    size_t n = std::min<size_t>(len, m->size - m->pos);
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return static_cast<int>(n);
}

int64_t io_seek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<MemIo*>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(m->size);
    int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = static_cast<int64_t>(m->pos); break;
        case SEEK_END: base = static_cast<int64_t>(m->size); break;
        default: return -1;
    }
    int64_t p = base + offset;
    if (p < 0 || p > static_cast<int64_t>(m->size)) return -1;
    m->pos = static_cast<size_t>(p);
    return p;
}

void append_start_code(std::vector<uint8_t>& v) { v.insert(v.end(), kStart, kStart + 4); }

// avcC -> SPS y PPS en Annex B (lo que MediaCodec espera en csd-0 y csd-1). nal_len: bytes del prefijo de longitud de
// cada NAL en las muestras (0: la muestra ya viene en Annex B).
bool parse_avcc(const uint8_t* e, int n, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps, int* nal_len) {
    if (n >= 4 && (!memcmp(e, kStart, 4) || (e[0] == 0 && e[1] == 0 && e[2] == 1))) {  // ya Annex B
        sps.assign(e, e + n);
        *nal_len = 0;
        return true;
    }
    if (n < 7 || e[0] != 1) return false;
    *nal_len = (e[4] & 3) + 1;
    int p = 5, count = e[p++] & 0x1f;
    for (int i = 0; i < count; i++) {
        if (p + 2 > n) return false;
        int len = (e[p] << 8) | e[p + 1];
        p += 2;
        if (p + len > n) return false;
        append_start_code(sps);
        sps.insert(sps.end(), e + p, e + p + len);
        p += len;
    }
    if (p >= n) return true;
    count = e[p++];
    for (int i = 0; i < count; i++) {
        if (p + 2 > n) return false;
        int len = (e[p] << 8) | e[p + 1];
        p += 2;
        if (p + len > n) return false;
        append_start_code(pps);
        pps.insert(pps.end(), e + p, e + p + len);
        p += len;
    }
    return true;
}
}  // namespace

struct Demuxer {
    std::mutex m;
    MemIo io{};
    AVIOContext* avio = nullptr;
    AVFormatContext* fmt = nullptr;
    AVPacket* pkt = nullptr;
    int map[kMaxTracks];   // pista expuesta -> stream
    int nal_len[kMaxTracks];
    int ntracks = 0;
    uint64_t selected = 0;  // bits de stream
    bool have = false, eof = false;
    std::vector<uint8_t> sample;  // la muestra actual, ya convertida
    int sample_track = -1;

    // Muestras leídas del contenedor y aún no entregadas, por pista. El orden de entrega es el de AMediaExtractor
    // (siempre la de menor tiempo entre las pistas elegidas), no el de posición en el archivo: con éste el audio
    // iba muchos frames por delante del vídeo y el juego se quedaba esperando.
    struct Pend {
        AVPacket* pkt = nullptr;
        std::vector<uint8_t> sample;
        int track = -1;
        int64_t time_us = 0;
    };
    std::deque<Pend> queue[kMaxTracks];

    void drop_queues() {
        for (auto& q : queue) {
            for (auto& p : q)
                if (p.pkt) av_packet_free(&p.pkt);
            q.clear();
        }
    }

    ~Demuxer() {
        drop_queues();
        if (pkt) av_packet_free(&pkt);
        if (fmt) avformat_close_input(&fmt);
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }

    // Lee del contenedor la próxima muestra de una pista elegida (ya convertida a Annex B). Con m cogido.
    bool read_one(Pend* out) {
        for (;;) {
            AVPacket* p = av_packet_alloc();
            if (!p) return false;
            if (av_read_frame(fmt, p) < 0) {
                av_packet_free(&p);
                return false;
            }
            int track = -1;
            for (int i = 0; i < ntracks; i++)
                if (map[i] == p->stream_index) track = i;
            if (track < 0 || !(selected & (1ull << p->stream_index))) {
                av_packet_free(&p);
                continue;
            }
            out->track = track;
            out->sample.clear();
            const uint8_t* d = p->data;
            int n = p->size;
            if (nal_len[track] > 0) {  // H.264 con prefijos de longitud -> Annex B
                int L = nal_len[track];
                int q = 0;
                while (q + L <= n) {
                    uint32_t len = 0;
                    for (int i = 0; i < L; i++) len = (len << 8) | d[q + i];
                    q += L;
                    if (len == 0 || q + static_cast<int64_t>(len) > n) break;
                    append_start_code(out->sample);
                    out->sample.insert(out->sample.end(), d + q, d + q + len);
                    q += static_cast<int>(len);
                }
            } else {
                out->sample.assign(d, d + n);
            }
            AVStream* st = fmt->streams[p->stream_index];
            int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
            int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
            out->time_us = ts == AV_NOPTS_VALUE ? 0 : av_rescale_q(ts - start, st->time_base, AVRational{1, 1000000});
            out->pkt = p;
            return true;
        }
    }

    // Deja en `pkt` y `sample` la muestra de menor tiempo entre las pistas elegidas. Con m cogido.
    void fill() {
        while (fmt && !have) {
            // Cada pista elegida necesita su próxima muestra en cola (salvo que el archivo se haya acabado).
            while (!eof) {
                bool need = false;
                for (int i = 0; i < ntracks; i++)
                    if ((selected & (1ull << map[i])) && queue[i].empty()) need = true;
                if (!need) break;
                Pend p;
                if (!read_one(&p)) {
                    eof = true;
                    break;
                }
                int t = p.track;
                queue[t].push_back(std::move(p));
            }
            int best = -1;
            for (int i = 0; i < ntracks; i++)
                if (!queue[i].empty() && (best < 0 || queue[i].front().time_us < queue[best].front().time_us)) best = i;
            if (best < 0) break;
            Pend p = std::move(queue[best].front());
            queue[best].pop_front();
            av_packet_unref(pkt);
            av_packet_move_ref(pkt, p.pkt);
            av_packet_free(&p.pkt);
            sample = std::move(p.sample);
            sample_track = p.track;
            have = true;
        }
    }
};

Demuxer* open(const uint8_t* data, size_t size) {
    if (!data || size == 0) return nullptr;
    auto* d = new Demuxer;
    d->io = {data, size, 0};
    const int kBuf = 64 * 1024;
    auto* buf = static_cast<uint8_t*>(av_malloc(kBuf));
    d->avio = buf ? avio_alloc_context(buf, kBuf, 0, &d->io, io_read, nullptr, io_seek) : nullptr;
    if (!d->avio) {
        if (buf) av_free(buf);
        delete d;
        return nullptr;
    }
    d->fmt = avformat_alloc_context();
    if (!d->fmt) {
        delete d;
        return nullptr;
    }
    d->fmt->pb = d->avio;
    d->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    int r = avformat_open_input(&d->fmt, "movie.mp4", nullptr, nullptr);
    if (r < 0) {
        printf("[movie] ffmpeg: no abre el vídeo (%d)\n", r);
        delete d;  // (avformat_open_input ya liberó fmt al fallar)
        return nullptr;
    }
    if (avformat_find_stream_info(d->fmt, nullptr) < 0) {
        printf("[movie] ffmpeg: find_stream_info falló\n");
        delete d;
        return nullptr;
    }
    d->pkt = av_packet_alloc();
    for (unsigned i = 0; i < d->fmt->nb_streams && d->ntracks < kMaxTracks; i++) {
        AVCodecParameters* p = d->fmt->streams[i]->codecpar;
        printf("[movie] ffmpeg: stream %u tipo %d códec %s %dx%d\n", i, static_cast<int>(p->codec_type),
               avcodec_get_name(p->codec_id), p->width, p->height);
        bool video = p->codec_type == AVMEDIA_TYPE_VIDEO && p->codec_id == AV_CODEC_ID_H264;
        bool audio = p->codec_type == AVMEDIA_TYPE_AUDIO && p->codec_id == AV_CODEC_ID_AAC;
        if (!video && !audio) continue;  // los demás códecs no los sabe MediaCodec con este formato
        d->nal_len[d->ntracks] = 0;
        if (video) {
            std::vector<uint8_t> sps, pps;
            int nl = 0;
            if (p->extradata && parse_avcc(p->extradata, p->extradata_size, sps, pps, &nl)) d->nal_len[d->ntracks] = nl;
        }
        d->map[d->ntracks++] = static_cast<int>(i);
    }
    printf("[movie] ffmpeg: %d pistas utilizables\n", d->ntracks);
    if (d->ntracks == 0) {
        delete d;
        return nullptr;
    }
    return d;
}

void destroy(Demuxer* d) { delete d; }

int track_count(Demuxer* d) { return d ? d->ntracks : 0; }

AMediaFormat* track_format(Demuxer* d, int track) {
    if (!d || track < 0 || track >= d->ntracks) return nullptr;
    AVStream* s = d->fmt->streams[d->map[track]];
    AVCodecParameters* p = s->codecpar;
    AMediaFormat* f = AMediaFormat_new();
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, p->width);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, p->height);
        int fps = s->avg_frame_rate.den ? s->avg_frame_rate.num / s->avg_frame_rate.den : 30;
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, fps > 0 ? fps : 30);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 4 << 20);
        std::vector<uint8_t> sps, pps;
        int nl = 0;
        if (p->extradata && parse_avcc(p->extradata, p->extradata_size, sps, pps, &nl)) {
            if (!sps.empty()) AMediaFormat_setBuffer(f, "csd-0", sps.data(), sps.size());
            if (!pps.empty()) AMediaFormat_setBuffer(f, "csd-1", pps.data(), pps.size());
        }
    } else {
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, p->sample_rate);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, p->ch_layout.nb_channels);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 256 << 10);
        if (p->extradata && p->extradata_size > 0) AMediaFormat_setBuffer(f, "csd-0", p->extradata, p->extradata_size);
    }
    int64_t dur = s->duration != AV_NOPTS_VALUE ? av_rescale_q(s->duration, s->time_base, AVRational{1, 1000000})
                                                : d->fmt->duration;
    if (dur > 0) AMediaFormat_setInt64(f, AMEDIAFORMAT_KEY_DURATION, dur);
    return f;
}

bool select_track(Demuxer* d, int track, bool on) {
    if (!d || track < 0 || track >= d->ntracks) return false;
    std::lock_guard lock(d->m);
    uint64_t bit = 1ull << d->map[track];
    if (on) d->selected |= bit;
    else d->selected &= ~bit;
    return true;
}

int next_track(Demuxer* d) {
    std::lock_guard lock(d->m);
    d->fill();
    return d->have ? d->sample_track : -1;
}

int64_t sample_time(Demuxer* d) {
    std::lock_guard lock(d->m);
    d->fill();
    if (!d->have) return -1;
    AVStream* s = d->fmt->streams[d->pkt->stream_index];
    int64_t ts = d->pkt->pts != AV_NOPTS_VALUE ? d->pkt->pts : d->pkt->dts;
    if (ts == AV_NOPTS_VALUE) return 0;
    int64_t start = s->start_time != AV_NOPTS_VALUE ? s->start_time : 0;
    return av_rescale_q(ts - start, s->time_base, AVRational{1, 1000000});
}

bool sample_sync(Demuxer* d) {
    std::lock_guard lock(d->m);
    d->fill();
    return d->have && (d->pkt->flags & AV_PKT_FLAG_KEY);
}

ssize_t sample_size(Demuxer* d) {
    std::lock_guard lock(d->m);
    d->fill();
    return d->have ? static_cast<ssize_t>(d->sample.size()) : -1;
}

ssize_t read_sample(Demuxer* d, uint8_t* out, size_t capacity) {
    std::lock_guard lock(d->m);
    d->fill();
    if (!d->have || d->sample.size() > capacity) return -1;
    memcpy(out, d->sample.data(), d->sample.size());
    return static_cast<ssize_t>(d->sample.size());
}

bool advance(Demuxer* d) {
    std::lock_guard lock(d->m);
    if (d->have) {
        av_packet_unref(d->pkt);
        d->have = false;
    }
    d->fill();
    return d->have;
}

bool seek(Demuxer* d, int64_t time_us, bool forward) {
    std::lock_guard lock(d->m);
    if (d->have) {
        av_packet_unref(d->pkt);
        d->have = false;
    }
    d->eof = false;
    d->drop_queues();
    return av_seek_frame(d->fmt, -1, time_us, forward ? 0 : AVSEEK_FLAG_BACKWARD) >= 0;
}

int64_t duration_us(Demuxer* d) {
    int64_t end = 0;
    for (int i = 0; i < d->ntracks; i++) {
        AVStream* s = d->fmt->streams[d->map[i]];
        int64_t dur = s->duration != AV_NOPTS_VALUE ? av_rescale_q(s->duration, s->time_base, AVRational{1, 1000000})
                                                    : d->fmt->duration;
        end = std::max(end, dur);
    }
    return end;
}

struct SwVideo {
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int errors = 0;
};

void sw_close(SwVideo* s) {
    if (!s) return;
    if (s->ctx) avcodec_free_context(&s->ctx);
    if (s->pkt) av_packet_free(&s->pkt);
    if (s->frame) av_frame_free(&s->frame);
    delete s;
}

SwVideo* sw_open(const uint8_t* csd0, size_t n0, const uint8_t* csd1, size_t n1) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        printf("[movie] ffmpeg: sin decoder H.264\n");
        return nullptr;
    }
    auto* s = new SwVideo;
    s->ctx = avcodec_alloc_context3(codec);
    s->pkt = av_packet_alloc();
    s->frame = av_frame_alloc();
    if (!s->ctx || !s->pkt || !s->frame) {
        sw_close(s);
        return nullptr;
    }
    size_t total = (csd0 ? n0 : 0) + (csd1 ? n1 : 0);
    if (total) {
        s->ctx->extradata = static_cast<uint8_t*>(av_mallocz(total + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!s->ctx->extradata) {
            sw_close(s);
            return nullptr;
        }
        size_t o = 0;
        if (csd0 && n0) memcpy(s->ctx->extradata + o, csd0, n0), o += n0;
        if (csd1 && n1) memcpy(s->ctx->extradata + o, csd1, n1), o += n1;
        s->ctx->extradata_size = static_cast<int>(total);
    }
    s->ctx->pkt_timebase = AVRational{1, 1000000};
    s->ctx->thread_count = 1;
    int r = avcodec_open2(s->ctx, codec, nullptr);
    if (r < 0) {
        printf("[movie] ffmpeg: avcodec_open2 falló (%d)\n", r);
        sw_close(s);
        return nullptr;
    }
    return s;
}

void sw_flush(SwVideo* s) {
    if (s && s->ctx) avcodec_flush_buffers(s->ctx);
}

void sw_decode(SwVideo* s, const uint8_t* data, size_t size, int64_t pts_us, std::vector<SwFrame>* out) {
    if (!s || !s->ctx) return;
    int r;
    if (data) {
        s->pkt->data = const_cast<uint8_t*>(data);
        s->pkt->size = static_cast<int>(size);
        s->pkt->pts = pts_us;
        s->pkt->dts = pts_us;
        r = avcodec_send_packet(s->ctx, s->pkt);
        s->pkt->data = nullptr;
        s->pkt->size = 0;
    } else {
        r = avcodec_send_packet(s->ctx, nullptr);
    }
    if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF && s->errors++ < 8)
        printf("[movie] ffmpeg: send_packet %d\n", r);
    while (avcodec_receive_frame(s->ctx, s->frame) >= 0) {
        AVFrame* f = s->frame;
        int w = f->width & ~1, h = f->height & ~1;
        bool nv12 = f->format == AV_PIX_FMT_NV12;
        bool i420 = f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P;
        if ((nv12 || i420) && w > 0 && h > 0) {
            SwFrame o;
            o.width = w;
            o.height = h;
            o.pts_us = f->pts != AV_NOPTS_VALUE ? f->pts : (f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : pts_us);
            o.nv12.resize(size_t(w) * h * 3 / 2);
            uint8_t* y = o.nv12.data();
            for (int row = 0; row < h; row++) memcpy(y + size_t(row) * w, f->data[0] + size_t(row) * f->linesize[0], w);
            uint8_t* uv = y + size_t(w) * h;
            for (int row = 0; row < h / 2; row++, uv += w) {
                if (nv12) {
                    memcpy(uv, f->data[1] + size_t(row) * f->linesize[1], w);
                } else {
                    const uint8_t* u = f->data[1] + size_t(row) * f->linesize[1];
                    const uint8_t* v = f->data[2] + size_t(row) * f->linesize[2];
                    for (int x = 0; x < w / 2; x++) uv[2 * x] = u[x], uv[2 * x + 1] = v[x];
                }
            }
            out->push_back(std::move(o));
        } else if (s->errors++ < 8) {
            printf("[movie] ffmpeg: formato de pixel %d no soportado\n", f->format);
        }
        av_frame_unref(f);
    }
}

#endif  // MHRISE_FFMPEG

}  // namespace movie_ff
