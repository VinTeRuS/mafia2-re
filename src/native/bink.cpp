// binkw32.dll replacement: Bink videos (.bik) decoded with FFmpeg (libavformat's bink demuxer, libavcodec's binkvideo).
// The game uses the "frame buffers" flavour of the Bink API: BinkGetFrameBuffersInfo tells it how large the Y / cR / cB
// planes are, it allocates them (D3D textures it locks) and hands them back with BinkRegisterFrameBuffers; BinkDoFrame
// then decodes the current frame into the registered planes, BinkNextFrame advances, BinkWait paces by the frame rate.
// Audio: not played yet (the audio tracks are skipped); the video clock is the wall clock.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "win.h"

#ifdef MAFIA2RE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}
#endif

namespace native {

// Open full-screen movies (not the menu backgrounds), for the touch controls (m2_script.cpp, nativeGameState).
std::atomic<int> g_movies_open{0};
std::atomic<int> g_menu_videos_open{0};  // the menu backgrounds: the main menu is up (autoplay wait_menu)

namespace {

// BINK record fields the game reads (bink.h): Width, Height, Frames, FrameNum, LastFrameNum, FrameRate, FrameRateDiv,
// ReadError, OpenFlags, ... The record is 0x200 bytes; our own state pointer lives in the last dword.
constexpr uint32_t kBinkSize = 0x200;
constexpr uint32_t kOffHost = 0x1fc;

struct Bink {
#ifdef MAFIA2RE_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int vstream = -1;
#endif
    uint32_t guest = 0, width = 0, height = 0, frames = 0;
    uint32_t fb_guest = 0;          // BINKFRAMEBUFFERS registered by the game
    int decoded = 0;                // frames decoded so far
    int next_set = 0;
    bool paused = false, eof = false;
    bool movie = false;  // counted in g_movies_open
    std::chrono::steady_clock::time_point start;
    bool started = false;
    uint32_t rate = 30000, rate_div = 1000;
};

std::mutex g_mutex;
std::map<uint32_t, Bink*> g_binks;

Bink* Find(uint32_t guest) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_binks.find(guest);
    return it == g_binks.end() ? nullptr : it->second;
}

uint32_t Align16(uint32_t v) { return (v + 15) & ~15u; }

#ifdef MAFIA2RE_FFMPEG
// Next video frame into b->frame (decoded in decode order); false at the end of the file.
bool DecodeNext(Bink* b) {
    while (true) {
        int r = avcodec_receive_frame(b->vdec, b->frame);
        if (r == 0) return true;
        if (r != AVERROR(EAGAIN)) return false;
        r = av_read_frame(b->fmt, b->pkt);
        if (r < 0) {
            avcodec_send_packet(b->vdec, nullptr);  // flush
            r = avcodec_receive_frame(b->vdec, b->frame);
            return r == 0;
        }
        if (b->pkt->stream_index == b->vstream) avcodec_send_packet(b->vdec, b->pkt);
        av_packet_unref(b->pkt);
    }
}
#endif

// ---- the API (stdcall) ----
// The game installs its own file I/O with BinkSetIO and passes names relative to its video folders; find the file in the
// video folders of the install and its DLCs.
std::string ResolveBink(const char* name) {
    std::string path = HostPath(name);
    if (access(path.c_str(), R_OK) == 0) return path;
    std::string base = name;
    if (size_t k = base.find_last_of("\\/"); k != std::string::npos) base = base.substr(k + 1);
    static const char* kDirs[] = {"C:\\Game\\pc\\sds\\video\\", "C:\\Game\\pc\\dlcs\\cnt_jimmys_vendetta\\sds\\video\\",
                                  "C:\\Game\\pc\\dlcs\\cnt_betrayal_of_jimmy\\sds\\video\\"};
    for (const char* d : kDirs) {
        std::string p = HostPath((std::string(d) + base).c_str());
        if (access(p.c_str(), R_OK) == 0) return p;
    }
    return path;
}

void k_BinkOpen(Ctx* c) {  // HBINK BinkOpen(const char* name, u32 flags)
    std::string path = ResolveBink(Str(Arg(c, 0)));
    uint32_t flags = Arg(c, 1);
#ifdef MAFIA2RE_FFMPEG
    if (getenv("MAFIA2RE_NO_VIDEO")) {  // test what a build without FFmpeg does
        Return(c, 0, 2);
        return;
    }
    auto* b = new Bink();
    if (avformat_open_input(&b->fmt, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(b->fmt, nullptr) < 0) {
        Log("BinkOpen(%s): cannot open", path.c_str());
        if (b->fmt) avformat_close_input(&b->fmt);
        delete b;
        Return(c, 0, 2);
        return;
    }
    for (unsigned i = 0; i < b->fmt->nb_streams; ++i) {
        if (b->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            b->vstream = int(i);
            break;
        }
    }
    if (b->vstream < 0) {
        avformat_close_input(&b->fmt);
        delete b;
        Return(c, 0, 2);
        return;
    }
    AVStream* vs = b->fmt->streams[b->vstream];
    const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
    b->vdec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(b->vdec, vs->codecpar);
    if (!codec || avcodec_open2(b->vdec, codec, nullptr) < 0) {
        Log("BinkOpen(%s): no decoder", path.c_str());
        avcodec_free_context(&b->vdec);
        avformat_close_input(&b->fmt);
        delete b;
        Return(c, 0, 2);
        return;
    }
    b->frame = av_frame_alloc();
    b->pkt = av_packet_alloc();
    b->width = uint32_t(vs->codecpar->width);
    b->height = uint32_t(vs->codecpar->height);
    int64_t n = vs->nb_frames;
    if (n <= 0 && vs->duration > 0) n = vs->duration;  // the bink demuxer counts the duration in frames
    if (n <= 0) n = 1000000;
    b->frames = uint32_t(n);
    AVRational fr = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        b->rate = uint32_t(fr.num);
        b->rate_div = uint32_t(fr.den);
    }
    b->guest = HeapAlloc(kBinkSize);
    memset(Ptr<void>(b->guest), 0, kBinkSize);
    W32(b->guest + 0, b->width);
    W32(b->guest + 4, b->height);
    W32(b->guest + 8, b->frames);
    W32(b->guest + 12, 1);  // FrameNum (1-based)
    W32(b->guest + 16, 0);  // LastFrameNum
    W32(b->guest + 20, b->rate);
    W32(b->guest + 24, b->rate_div);
    W32(b->guest + 32, flags);  // OpenFlags
    W32(b->guest + kOffHost, 0);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_binks[b->guest] = b;
    }
    std::string lower = path;
    for (char& ch : lower) ch = char(tolower((unsigned char)ch));
    b->movie = lower.find("menu") == std::string::npos;
    if (b->movie) ++g_movies_open;
    else ++g_menu_videos_open;
    Log("BinkOpen(%s, flags %x) -> %08x: %ux%u, %u frames, %u/%u fps", path.c_str(), flags, b->guest, b->width, b->height,
        b->frames, b->rate, b->rate_div);
    Return(c, b->guest, 2);
#else
    (void)path;
    (void)flags;
    Return(c, 0, 2);  // no FFmpeg in this build: movies fail to open
#endif
}

void k_BinkClose(Ctx* c) {
    uint32_t g = Arg(c, 0);
    Bink* b = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_binks.find(g);
        if (it != g_binks.end()) {
            b = it->second;
            g_binks.erase(it);
        }
    }
    if (b) {
        if (b->movie) --g_movies_open;
        else --g_menu_videos_open;
#ifdef MAFIA2RE_FFMPEG
        av_frame_free(&b->frame);
        av_packet_free(&b->pkt);
        avcodec_free_context(&b->vdec);
        avformat_close_input(&b->fmt);
#endif
        HeapFree(g);
        delete b;
    }
    Return(c, 0, 1);
}

// BINKFRAMEBUFFERS: TotalFrames, YABufferWidth/Height, cRcBBufferWidth/Height, FrameNum (+0x14: the set the last BinkDoFrame
// decoded into; the game reads it to pick the planes it uploads), then two plane sets (at +0x18)
// (48 bytes each) of four planes {u32 Allocate; void* Buffer; u32 BufferPitch;} (Y, cR, cB, A). Bink sets Allocate for the
// planes the application has to provide; the application allocates them and fills Buffer and BufferPitch.
void k_BinkGetFrameBuffersInfo(Ctx* c) {
    Bink* b = Find(Arg(c, 0));
    uint32_t fb = Arg(c, 1);
    if (!b || !fb) { Return(c, 0, 2); return; }
    for (uint32_t i = 0; i < 0x18 + 96; i += 4) W32(fb + i, 0);
    W32(fb + 0, 2);                        // TotalFrames: two buffer sets
    W32(fb + 4, Align16(b->width));        // YABufferWidth
    W32(fb + 8, Align16(b->height));       // YABufferHeight
    W32(fb + 12, Align16(b->width) / 2);   // cRcBBufferWidth
    W32(fb + 16, Align16(b->height) / 2);  // cRcBBufferHeight
    for (uint32_t set = 0; set < 2; ++set)
        for (uint32_t plane = 0; plane < 3; ++plane) W32(fb + 0x18 + 48 * set + 12 * plane, 1);  // Allocate (no alpha plane)
    Return(c, 1, 2);
}

void k_BinkRegisterFrameBuffers(Ctx* c) {
    if (Bink* b = Find(Arg(c, 0))) b->fb_guest = Arg(c, 1);
    Return(c, 0, 2);
}

void k_BinkDoFrame(Ctx* c) {
    Bink* b = Find(Arg(c, 0));
    if (b && !b->started) {
        b->started = true;
        b->start = std::chrono::steady_clock::now();
    }
#ifdef MAFIA2RE_FFMPEG
    if (b && b->fb_guest && !b->eof) {
        if (!DecodeNext(b)) {
            b->eof = true;
        } else {
            uint32_t set = uint32_t(b->next_set);
            b->next_set ^= 1;
            uint32_t planes = b->fb_guest + 0x18 + 48 * set;  // Y, cR, cB, A: {Allocate, Buffer, BufferPitch}
            // AVFrame: data[0] = Y, data[1] = Cb, data[2] = Cr. Bink's "cR"/"cB" planes: V then U.
            struct Src { const uint8_t* p; int stride; uint32_t w, h; } src[3] = {
                {b->frame->data[0], b->frame->linesize[0], b->width, b->height},
                {b->frame->data[2], b->frame->linesize[2], b->width / 2, b->height / 2},  // cR = V
                {b->frame->data[1], b->frame->linesize[1], b->width / 2, b->height / 2},  // cB = U
            };
            for (int p = 0; p < 3; ++p) {
                uint32_t dst = R32(planes + 12 * uint32_t(p) + 4), pitch = R32(planes + 12 * uint32_t(p) + 8);
                if (!dst) continue;
                for (uint32_t y = 0; y < src[p].h; ++y) memcpy(Ptr<uint8_t>(dst + y * pitch), src[p].p + size_t(y) * size_t(src[p].stride), src[p].w);
            }
            if (uint32_t a = R32(planes + 36 + 4); a) {  // alpha plane provided: opaque
                uint32_t pitch = R32(planes + 36 + 8);
                for (uint32_t y = 0; y < b->height; ++y) memset(Ptr<uint8_t>(a + y * pitch), 255, b->width);
            }
            W32(b->fb_guest + 0x14, set);  // FrameNum: the set just decoded
            ++b->decoded;
        }
    }
#endif
    Return(c, 0, 1);
}

void k_BinkNextFrame(Ctx* c) {
    uint32_t g = Arg(c, 0);
    if (Bink* b = Find(g)) {
        uint32_t cur = R32(g + 12);
        W32(g + 16, cur);
        if (cur < b->frames && !b->eof) W32(g + 12, cur + 1);
    }
    Return(c, 0, 1);
}

// Nonzero while the next frame is not due yet.
void k_BinkWait(Ctx* c) {
    Bink* b = Find(Arg(c, 0));
    uint32_t r = 0;
    if (b && b->started && !b->paused) {
        uint32_t cur = R32(b->guest + 12);
        double due = double(cur - 1) * double(b->rate_div) / double(b->rate);
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - b->start).count();
        r = now < due ? 1 : 0;
    }
    Return(c, r, 1);
}

void k_BinkPause(Ctx* c) {
    if (Bink* b = Find(Arg(c, 0))) {
        bool pause = Arg(c, 1) != 0;
        if (b->paused && !pause) b->start = std::chrono::steady_clock::now() - std::chrono::microseconds(
            int64_t(double(R32(b->guest + 12) - 1) * double(b->rate_div) / double(b->rate) * 1e6));
        b->paused = pause;
    }
    Return(c, 0, 2);
}

void k_One1(Ctx* c) { Return(c, 1, 1); }
void k_One2(Ctx* c) { Return(c, 1, 2); }
void k_One3(Ctx* c) { Return(c, 1, 3); }
void k_Zero1(Ctx* c) { Return(c, 0, 1); }
void k_Zero2(Ctx* c) { Return(c, 0, 2); }

const ImportDef kBink[] = {
    {"binkw32.dll", "_BinkOpen@8", k_BinkOpen},
    {"binkw32.dll", "_BinkClose@4", k_BinkClose},
    {"binkw32.dll", "_BinkGetFrameBuffersInfo@8", k_BinkGetFrameBuffersInfo},
    {"binkw32.dll", "_BinkRegisterFrameBuffers@8", k_BinkRegisterFrameBuffers},
    {"binkw32.dll", "_BinkDoFrame@4", k_BinkDoFrame},
    {"binkw32.dll", "_BinkNextFrame@4", k_BinkNextFrame},
    {"binkw32.dll", "_BinkWait@4", k_BinkWait},
    {"binkw32.dll", "_BinkPause@8", k_BinkPause},
    {"binkw32.dll", "_BinkShouldSkip@4", k_Zero1},
    {"binkw32.dll", "_BinkSetVolume@12", k_One3},
    {"binkw32.dll", "_BinkSetSoundTrack@8", k_One2},
    {"binkw32.dll", "_BinkGetTrackID@8", k_Zero2},
    {"binkw32.dll", "_BinkOpenDirectSound@4", k_Zero1},
    {"binkw32.dll", "_BinkSetSoundSystem@8", k_One2},
    {"binkw32.dll", "_BinkSetIO@4", k_One1},
    {"binkw32.dll", "_BinkSetIOSize@4", k_One1},
    {"binkw32.dll", "_BinkSetMemory@8", k_One2},
};
ImportRegistrar g_bink(kBink);

}  // namespace

}  // namespace native
