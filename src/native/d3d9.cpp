#include <time.h>
#include <sys/stat.h>
// D3D9 for the native host: the engine's COM calls are forwarded to DXVK (D3D9 on Vulkan, built natively).
//
// Guest objects are 16-byte blocks in guest memory: [0] guest vtable (thunk addresses), [4] index of the host
// object. Method handlers are generated from d3d9.h (tools/gen_d3d9_bridge.py -> d3d9_gen.inc); this file holds the
// object table and the methods whose arguments need conversion: structures with window handles (present
// parameters) and Lock/Unlock, where the host's mapped pointer lies outside guest memory and a guest shadow buffer
// is copied in and out.
#include <SDL2/SDL.h>
#include <d3d9.h>

#include <unistd.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "win.h"

void PinToBigCores();  // main.cpp
void PinDxvkThreads();  // main.cpp
namespace native {

SDL_Window* g_sdl_window = nullptr;
uint32_t HostWindow();
void ClientSize(int* w, int* h);     // user32.cpp
bool PresentRect(int* x, int* y, int* w, int* h);  // user32.cpp
void SetClientSize(int w, int h);

namespace {

struct MethodDef {
    const char* name;
    ImportFn fn;
};
struct InterfaceDef {
    const char* name;
    const MethodDef* methods;
    size_t count;
};

std::recursive_mutex g_mutex;
// index -> host object, read without a lock by every bridged call: fixed chunks that never move, appended to
// under g_mutex.
class HostTable {
public:
    void* Get(uint32_t i) const {
        return chunks_[i >> kBits].load(std::memory_order_acquire)[i & kMask].load(std::memory_order_acquire);
    }
    void Set(uint32_t i, void* p) {
        chunks_[i >> kBits].load(std::memory_order_relaxed)[i & kMask].store(p, std::memory_order_release);
    }
    // True when i names a slot that was ever handed out (a guest "object" that is no wrapper of ours has garbage there).
    bool Valid(uint32_t i) const {
        return i < size_ && chunks_[i >> kBits].load(std::memory_order_acquire) != nullptr;
    }
    uint32_t Append(void* p) {
        uint32_t i = size_;
        if ((i >> kBits) >= kChunks) Fatal("d3d9: too many objects");
        if (!chunks_[i >> kBits].load(std::memory_order_relaxed)) {
            chunks_[i >> kBits].store(new std::atomic<void*>[kMask + 1](), std::memory_order_release);
        }
        Set(i, p);
        ++size_;
        return i;
    }

private:
    static constexpr uint32_t kBits = 14, kMask = (1u << kBits) - 1, kChunks = 4096;
    std::atomic<std::atomic<void*>*> chunks_[kChunks] = {};
    uint32_t size_ = 0;
};
HostTable g_hosts;
// host object -> guest object. Child surfaces die with their texture without a Release through the bridge, so an
// entry can be stale: a new object at the same address with another interface must not reuse it.
std::unordered_map<void*, uint32_t> g_guest_of;
std::map<std::string, uint32_t> g_vtables;       // interface name -> guest vtable

template <typename T>
T* Host(uint32_t guest) {
    return static_cast<T*>(g_hosts.Get(R32(guest + 4)));
}

// An object pointer the game passes as an argument (SetTexture, SetStreamSource, ...). Battlefront II sometimes hands over a
// stale raster's pointer that is no wrapper at all: logged and treated as NULL (valid for those calls) instead of
// reading a host slot out of garbage.
template <typename T>
T* Unwrap(uint32_t guest) {
    if (!guest) return nullptr;
    uint32_t index = R32(guest + 4);
    if (!g_hosts.Valid(index)) {
        static std::atomic<int> logged{0};
        if (logged++ < 20) Log("d3d9: %08x is not a D3D object (vtable %08x, index %08x); used as NULL", guest, R32(guest), index);
        return nullptr;
    }
    return Host<T>(guest);
}

uint32_t WrapAs(void* host, const char* iface) {
    if (!host) return 0;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto vt = g_vtables.find(iface);
    if (vt == g_vtables.end()) Fatal("d3d9: no vtable for %s", iface);
    auto it = g_guest_of.find(host);
    if (it != g_guest_of.end() && R32(it->second) == vt->second) return it->second;
    uint32_t g = HeapAlloc(16);
    W32(g, vt->second);
    W32(g + 4, g_hosts.Append(host));
    g_guest_of[host] = g;
    return g;
}

void DeadQueryInterface(Ctx* c) {
    if (Arg(c, 2)) W32(Arg(c, 2), 0);
    Return(c, E_NOINTERFACE, 3);
}
void DeadAddRef(Ctx* c) { Return(c, 0, 1); }
void DeadRelease(Ctx* c) {
    static int logged = 0;
    if (logged++ < 20) Log("d3d9: Release of an already released object %08x (from %08x) ignored", Arg(c, 0), R32(c->esp));
    Return(c, 0, 1);
}
void DeadMethod(Ctx* c) { Fatal("d3d9: method call on a released object %08x (from %08x)", Arg(c, 0), R32(c->esp)); }
uint32_t DeadVtable() {
    static uint32_t vt = [] {
        static const ImportDef defs[] = {{"d3d9.com", "Dead::QueryInterface", DeadQueryInterface},
                                         {"d3d9.com", "Dead::AddRef", DeadAddRef},
                                         {"d3d9.com", "Dead::Release", DeadRelease},
                                         {"d3d9.com", "Dead::Method", DeadMethod}};
        RegisterImports(defs, 4);
        uint32_t t = HeapAlloc(4 * 128);
        for (uint32_t k = 0; k < 128; ++k) W32(t + 4 * k, ImportThunk("d3d9.com", defs[k < 3 ? k : 3].name));
        return t;
    }();
    return vt;
}

void Forget(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t index = R32(guest + 4);
    auto it = g_guest_of.find(g_hosts.Get(index));
    if (it != g_guest_of.end() && it->second == guest) g_guest_of.erase(it);
    g_hosts.Set(index, nullptr);
    // The guest block is not reused. Its vtable becomes the dead one: a late Release or AddRef from the game (the
    // engine releases some rasters once more than it acquired, which Windows tolerates for objects the device still
    // holds) does nothing, and any other method stops with a clear message instead of jumping through garbage.
    W32(guest, DeadVtable());
}

uint32_t Wrap(IDirect3D9* p) { return WrapAs(p, "IDirect3D9"); }
uint32_t Wrap(IDirect3DDevice9* p) { return WrapAs(p, "IDirect3DDevice9"); }
uint32_t Wrap(IDirect3DSwapChain9* p) { return WrapAs(p, "IDirect3DSwapChain9"); }
uint32_t Wrap(IDirect3DSurface9* p) { return WrapAs(p, "IDirect3DSurface9"); }
uint32_t Wrap(IDirect3DVolume9* p) { return WrapAs(p, "IDirect3DVolume9"); }
uint32_t Wrap(IDirect3DVertexBuffer9* p) { return WrapAs(p, "IDirect3DVertexBuffer9"); }
uint32_t Wrap(IDirect3DIndexBuffer9* p) { return WrapAs(p, "IDirect3DIndexBuffer9"); }
uint32_t Wrap(IDirect3DTexture9* p) { return WrapAs(p, "IDirect3DTexture9"); }
uint32_t Wrap(IDirect3DCubeTexture9* p) { return WrapAs(p, "IDirect3DCubeTexture9"); }
uint32_t Wrap(IDirect3DVolumeTexture9* p) { return WrapAs(p, "IDirect3DVolumeTexture9"); }
uint32_t Wrap(IDirect3DVertexDeclaration9* p) { return WrapAs(p, "IDirect3DVertexDeclaration9"); }
uint32_t Wrap(IDirect3DVertexShader9* p) { return WrapAs(p, "IDirect3DVertexShader9"); }
uint32_t Wrap(IDirect3DPixelShader9* p) { return WrapAs(p, "IDirect3DPixelShader9"); }
uint32_t Wrap(IDirect3DStateBlock9* p) { return WrapAs(p, "IDirect3DStateBlock9"); }
uint32_t Wrap(IDirect3DQuery9* p) { return WrapAs(p, "IDirect3DQuery9"); }
uint32_t Wrap(IDirect3DBaseTexture9* p) {
    if (!p) return 0;
    switch (p->GetType()) {
        case D3DRTYPE_CUBETEXTURE: return WrapAs(p, "IDirect3DCubeTexture9");
        case D3DRTYPE_VOLUMETEXTURE: return WrapAs(p, "IDirect3DVolumeTexture9");
        default: return WrapAs(p, "IDirect3DTexture9");
    }
}

HWND HostHwnd(uint32_t guest_hwnd) { return guest_hwnd ? reinterpret_cast<HWND>(g_sdl_window) : nullptr; }

void ReturnFStd(Ctx* c, float v, int args) {
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
    c->ret_target = Pop32(c);
    c->esp += 4 * args;
}

void ForgetResource(void* host);  // lock shadows and texture adaptation state of a released object
extern std::shared_mutex g_copy_rw;
std::atomic<uint64_t> g_shader_create_ns{0};  // the game's shader creation: time inside DXVK, count
std::atomic<uint32_t> g_shader_creates{0};
// Hitch report: what happened during a long frame (shaders and textures created, lock bytes copied, file bytes read).
std::atomic<uint32_t> g_hitch_shaders{0}, g_hitch_textures{0};
std::atomic<uint64_t> g_hitch_copied{0};
uint32_t g_frame_draws = 0, g_frame_shadow_draws = 0;  // render thread: draws this frame, of them in shadow map passes  // lock copies (shared) vs. releases that may free their memory (exclusive)

// Vertex and index buffers whose last reference the engine dropped live on for a few frames: the engine sometimes
// still locks a buffer right after releasing it, which on Windows works (the object outlives its last public
// reference while the device uses it) but would hit the bridge's null vtable here. The bridge keeps the engine's
// last reference and drops it at a later Present (or before Reset, which needs default-pool buffers gone).
struct Grave {
    uint32_t guest;
    IUnknown* host;
    uint32_t frame;
};
std::vector<Grave> g_graves;
uint32_t g_present_count = 0;
}  // namespace
std::atomic<uint64_t> g_frames{0};  // presented frames (watchdog, perf log)
extern std::atomic<float> g_fps;  // main.cpp: frames per second over the last second
namespace {
constexpr uint32_t kGraveFrames = 3;

ULONG RefCount(IUnknown* p) {
    p->AddRef();
    return p->Release();
}

void DrainGraves(bool all) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    size_t kept = 0;
    for (const Grave& gr : g_graves) {
        if (!all && g_present_count - gr.frame < kGraveFrames) {
            g_graves[kept++] = gr;
            continue;
        }
        if (RefCount(gr.host) == 1) {
            ForgetResource(gr.host);
            Forget(gr.guest);
        }
        std::unique_lock<std::shared_mutex> no_copies(g_copy_rw);
        gr.host->Release();  // revived by the engine meanwhile: its own references keep it
    }
    g_graves.resize(kept);
}

template <typename T>
void ReleaseImpl(Ctx* c) {
    uint32_t g = Arg(c, 0);
    T* host = Host<T>(g);
    if constexpr (std::is_same_v<T, IDirect3DVertexBuffer9> || std::is_same_v<T, IDirect3DIndexBuffer9>) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        for (const Grave& gr : g_graves) {
            if (gr.host == host && RefCount(host) == 1) {  // released again while already pending
                Return(c, 0, 1);
                return;
            }
        }
        if (RefCount(host) == 1) {
            g_graves.push_back({g, host, g_present_count});
            Return(c, 0, 1);
            return;
        }
    }
    ULONG r;
    {
        std::unique_lock<std::shared_mutex> no_copies(g_copy_rw);
        r = host->Release();
    }
    if (r == 0) {
        ForgetResource(host);
        Forget(g);
    }
    Return(c, r, 1);
}

template <typename T>
void QueryInterfaceImpl(Ctx* c) {
    uint32_t g = Arg(c, 0), out = Arg(c, 2);
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    if (riid == __uuidof(IUnknown) || riid == __uuidof(T)) {
        Host<T>(g)->AddRef();
        W32(out, g);
        Return(c, S_OK, 3);
        return;
    }
    Log("d3d9: QueryInterface for an unsupported interface");
    W32(out, 0);
    Return(c, E_NOINTERFACE, 3);
}

// ---- present parameters: 56 bytes in guest memory, hDeviceWindow at +28 ----
D3DPRESENT_PARAMETERS ToHost(uint32_t g) {
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = R32(g + 0);
    p.BackBufferHeight = R32(g + 4);
    p.BackBufferFormat = D3DFORMAT(R32(g + 8));
    p.BackBufferCount = R32(g + 12);
    p.MultiSampleType = D3DMULTISAMPLE_TYPE(R32(g + 16));
    p.MultiSampleQuality = R32(g + 20);
    p.SwapEffect = D3DSWAPEFFECT(R32(g + 24));
    p.hDeviceWindow = HostHwnd(R32(g + 28) ? R32(g + 28) : HostWindow());
    p.Windowed = R32(g + 32);
    p.EnableAutoDepthStencil = R32(g + 36);
    p.AutoDepthStencilFormat = D3DFORMAT(R32(g + 40));
    p.Flags = R32(g + 44);
    p.FullScreen_RefreshRateInHz = R32(g + 48);
    p.PresentationInterval = R32(g + 52);
    // Android: the SDL window already covers the screen and exclusive fullscreen has no meaning there.
#ifdef __ANDROID__
    const bool windowed = true;
#else
    const bool windowed = getenv("MAFIA2RE_WINDOWED") != nullptr;
#endif
    if (windowed) {
        p.Windowed = TRUE;
        p.FullScreen_RefreshRateInHz = 0;
    }
    return p;
}

void ToGuest(const D3DPRESENT_PARAMETERS& p, uint32_t g) {
    W32(g + 0, p.BackBufferWidth);
    W32(g + 4, p.BackBufferHeight);
    W32(g + 8, p.BackBufferFormat);
    W32(g + 12, p.BackBufferCount);
    W32(g + 16, p.MultiSampleType);
    W32(g + 20, p.MultiSampleQuality);
    W32(g + 24, p.SwapEffect);
    W32(g + 28, HostWindow());
    W32(g + 36, p.EnableAutoDepthStencil);
    W32(g + 40, p.AutoDepthStencilFormat);
    W32(g + 44, p.Flags);
    W32(g + 52, p.PresentationInterval);
}

// ---- lock shadows ----
struct Shadow {
    uint32_t guest = 0, capacity = 0, size = 0;
    void* host = nullptr;
    bool write_back = false;
};
std::map<std::pair<void*, uint32_t>, Shadow> g_shadows;  // (object, subresource) -> shadow

bool IsBlockCompressed(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 ||
           f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '1')) || f == D3DFORMAT(MAKEFOURCC('A', 'T', 'I', '2'));
}

// Maps the host's locked memory into a guest buffer: copies it in unless the lock discards or never overwrites
// (the guest then only writes), and remembers to copy it back on unlock unless read-only.
// `reach`: how far the game may write from the returned pointer without harm on Windows, where it points into the
// resource itself (buffers: to the end of the buffer, even past the locked range). The shadow covers all of it, so
// such writes land in it instead of the guest heap next to it; only the locked `size` is copied back.
// Copies between guest and host lock memory run outside g_mutex, in shared mode; a final Release of a resource takes
// this exclusively, so it waits for copies in flight instead of freeing the memory under them (Mali crashed in its
// driver with its own structures overwritten by 1.0f / fp16 1.0 values, after a device lost).
std::shared_mutex g_copy_rw;
std::atomic<uint64_t> g_shadow_bytes{0}, g_shadow_calls{0}, g_shadow_ns{0};  // copies between guest and host locks, for the perf line
// The copies run without g_mutex: a texture level of 4 MB copied by the engine's streaming thread must not hold up
// the render thread, whose buffer locks take the same mutex. A shadow belongs to one subresource, which the engine
// locks from one thread at a time and does not release while locked, so the fields read under the lock stay valid.
void ShadowCopy(void* dst, const void* src, uint32_t size) {
    auto t0 = std::chrono::steady_clock::now();
    memcpy(dst, src, size);
    g_hitch_copied.fetch_add(size, std::memory_order_relaxed);
    g_shadow_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_shadow_bytes += size;
}
uint32_t ShadowLock(void* object, uint32_t sub, void* host, uint32_t size, DWORD flags, uint32_t reach = 0) {
    uint32_t guest;
    std::shared_lock<std::shared_mutex> copying(g_copy_rw, std::defer_lock);
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Shadow& s = g_shadows[{object, sub}];
        uint32_t need = reach > size ? reach : size;
        if (s.capacity < need) {
            if (s.guest) HeapFree(s.guest);
            s.guest = HeapAlloc(need);
            s.capacity = need;
        }
        s.size = size;
        s.host = host;
        s.write_back = !(flags & D3DLOCK_READONLY);
        guest = s.guest;
        if (!(flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) copying.lock();  // taken under g_mutex: no gap
    }
    if (copying.owns_lock()) ShadowCopy(Ptr<void>(guest), host, size);
    ++g_shadow_calls;
    return guest;
}

void ShadowUnlock(void* object, uint32_t sub) {
    void* host;
    uint32_t guest, size;
    std::shared_lock<std::shared_mutex> copying(g_copy_rw, std::defer_lock);
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto it = g_shadows.find({object, sub});
        if (it == g_shadows.end() || !it->second.host) return;
        Shadow& s = it->second;
        host = s.host;
        guest = s.guest;
        size = s.write_back ? s.size : 0;
        s.host = nullptr;
        if (size) copying.lock();  // taken under g_mutex: a release cannot slip in before the copy
    }
    if (size) ShadowCopy(host, Ptr<void>(guest), size);
}

// Vertex and index buffers: a guest mirror of the whole buffer. Only the CPU writes D3D9 buffers, so once filled the
// mirror equals the buffer: a lock returns mirror + offset with no copy in (reading lock memory back is the slow
// direction on phone GPUs, write-combined), and an unlock copies just the locked range out. The game locks static
// buffers without DISCARD/NOOVERWRITE ~3000 times a second, ~100 KB each: that copy in was half of all lock traffic.
struct Mirror {
    uint32_t guest = 0, size = 0;
    bool valid = false;  // equals the buffer's contents
    void* host = nullptr;
    uint32_t offset = 0, length = 0;
    bool write_back = false;
};
std::unordered_map<void*, Mirror> g_mirrors;
std::atomic<uint64_t> g_mirror_fills{0};
// Returns 0 when the mirror cannot serve this lock (nested lock): the caller falls back to ShadowLock.
uint32_t MirrorLock(void* object, uint32_t buffer_size, uint32_t offset, uint32_t length, void* host, DWORD flags) {
    std::shared_lock<std::shared_mutex> copying(g_copy_rw, std::defer_lock);
    uint32_t guest;
    bool fill;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Mirror& m = g_mirrors[object];
        if (m.host) return 0;
        if (m.size != buffer_size) {
            if (m.guest) HeapFree(m.guest);
            m.guest = HeapAlloc(buffer_size);
            m.size = buffer_size;
            m.valid = false;
        }
        // A discarded buffer's old contents are gone anyway: the mirror's stand in for them.
        fill = !m.valid && !(flags & D3DLOCK_DISCARD);
        m.valid = true;
        m.host = host;
        m.offset = offset;
        m.length = length;
        m.write_back = !(flags & D3DLOCK_READONLY);
        guest = m.guest;
        if (fill) copying.lock();
    }
    if (fill) {  // once per buffer: the whole of it, from the mapping's start
        ShadowCopy(Ptr<void>(guest), static_cast<uint8_t*>(host) - offset, buffer_size);
        ++g_mirror_fills;
    }
    ++g_shadow_calls;
    return guest + offset;
}
// Returns false when the lock was not a mirror one.
bool MirrorUnlock(void* object) {
    std::shared_lock<std::shared_mutex> copying(g_copy_rw, std::defer_lock);
    void* host;
    uint32_t src, length;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto it = g_mirrors.find(object);
        if (it == g_mirrors.end() || !it->second.host) return false;
        Mirror& m = it->second;
        host = m.host;
        src = m.guest + m.offset;
        length = m.write_back ? m.length : 0;
        m.host = nullptr;
        if (length) copying.lock();
    }
    if (length) ShadowCopy(host, Ptr<void>(src), length);
    return true;
}

void ForgetShadows(void* object) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto mirror = g_mirrors.find(object);
    if (mirror != g_mirrors.end()) {
        if (mirror->second.guest) HeapFree(mirror->second.guest);
        g_mirrors.erase(mirror);
    }
    for (auto it = g_shadows.lower_bound({object, 0}); it != g_shadows.end() && it->first.first == object;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_shadows.erase(it);
    }
}

uint32_t RectRows(const D3DSURFACE_DESC& d, const RECT* r) {
    uint32_t h = r ? uint32_t(r->bottom - r->top) : d.Height;
    return IsBlockCompressed(d.Format) ? (h + 3) / 4 : h;
}

// LockRect common part: D3DLOCKED_RECT in guest memory is {INT Pitch; void* pBits} (8 bytes).
void LockRectCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_RECT& lr,
                    const D3DSURFACE_DESC& desc, const RECT* rect, DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        uint32_t size = uint32_t(lr.Pitch) * RectRows(desc, rect);
        static const bool stats = SettingInt("lock_stats", 0) != 0;
        if (stats) {  // texture locks: [whole|rect][readonly|discard|plain]
            static std::mutex m;
            static uint64_t n[6], bytes[6], calls = 0;
            std::lock_guard<std::mutex> l(m);
            int k = (rect ? 3 : 0) + ((flags & D3DLOCK_READONLY) ? 0 : (flags & D3DLOCK_DISCARD) ? 1 : 2);
            ++n[k];
            bytes[k] += size;
            if (++calls % 2000 == 0)
                Log("lockstats tex: whole ro %llu/%lluMB disc %llu/%lluMB plain %llu/%lluMB | rect ro %llu/%lluMB disc %llu/%lluMB plain %llu/%lluMB | last fmt %u pool %u usage %lx %ux%u",
                    (unsigned long long)n[0], (unsigned long long)(bytes[0] >> 20), (unsigned long long)n[1], (unsigned long long)(bytes[1] >> 20),
                    (unsigned long long)n[2], (unsigned long long)(bytes[2] >> 20), (unsigned long long)n[3], (unsigned long long)(bytes[3] >> 20),
                    (unsigned long long)n[4], (unsigned long long)(bytes[4] >> 20), (unsigned long long)n[5], (unsigned long long)(bytes[5] >> 20),
                    unsigned(desc.Format), unsigned(desc.Pool), (unsigned long)desc.Usage, desc.Width, desc.Height);
        }
        W32(out, uint32_t(lr.Pitch));
        W32(out + 4, ShadowLock(object, sub, lr.pBits, size, flags));
    }
    Return(c, uint32_t(hr), args);
}

// ---- texture adaptation for mobile GPUs ----
// Invisible to the engine, which keeps seeing the textures it asked for:
//   - mip skipping (setting texture_skip_mips): a large mipmapped texture is created `skip` levels smaller; the
//     guest's top levels go to a scratch buffer and are dropped. Halves texture resolution per level, quarters
//     memory and bandwidth.
//   - DXT decoding (setting decode_dxt, automatic when the GPU has no BC formats, e.g. Mali): DXT1/3/5 textures are
//     created as A8R8G8B8 and decoded on unlock.
struct TexAdapt {
    D3DFORMAT format;   // what the guest asked for
    UINT width, height;
    UINT skip;          // guest level L is host level L - skip; levels below skip are dropped
    bool decode;        // host texture is decode_format, guest writes DXT blocks
    D3DFORMAT decode_format;  // A8R8G8B8, or A1R5G5B5 for DXT1 (half the memory, keeps its 1-bit alpha)
};
std::unordered_map<void*, TexAdapt> g_adapt;  // host texture -> adaptation, only for adapted textures
int g_skip_mips = -1;                          // resolved on first texture
UINT g_skip_min_size = 512;                    // only textures at least this large (larger side) lose levels
bool g_decode_dxt = false;
bool g_decode_16bit = false;  // decode_16bit: DXT1 -> A1R5G5B5 (half the memory, but breaks models: off by default)

bool IsDxt(D3DFORMAT f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

// Bytes per 4x4 block (DXT) or per pixel (plain formats the adaptation handles); 0 = not adaptable.
UINT BlockBytes(D3DFORMAT f) {
    switch (f) {
        case D3DFMT_DXT1: return 8;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return 16;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 4;
        default: return 0;
    }
}

// Pitch and row count of a guest level (or of `rect` within it) in the guest's format.
void GuestLevelLayout(D3DFORMAT f, UINT w, UINT h, const RECT* rect, UINT* pitch, UINT* rows) {
    UINT rh = rect ? UINT(rect->bottom - rect->top) : h;
    if (IsDxt(f)) {
        *pitch = ((w + 3) / 4) * BlockBytes(f);
        *rows = (rh + 3) / 4;
    } else {
        *pitch = w * BlockBytes(f);
        *rows = rh;
    }
}

const TexAdapt* AdaptOf(void* host) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_adapt.find(host);
    return it == g_adapt.end() ? nullptr : &it->second;
}

// Decides how a texture of the guest's description is created on the host.
TexAdapt PlanTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format) {
    if (g_skip_mips < 0) {
#ifdef __ANDROID__
        g_skip_mips = SettingInt("texture_skip_mips", 2);  // Mali decodes DXT: memory, not quality, decides here
#else
        g_skip_mips = SettingInt("texture_skip_mips", 0);
#endif
#ifdef __ANDROID__
        g_skip_min_size = UINT(SettingInt("texture_skip_min_size", 256));
#else
        g_skip_min_size = UINT(SettingInt("texture_skip_min_size", 512));
#endif
        Log("textures: skip %d mip level(s) of textures >= %u, decode DXT %s", g_skip_mips, g_skip_min_size,
            g_decode_dxt ? "on" : "off");
    }
    TexAdapt a{format, w, h, 0, false, D3DFMT_A8R8G8B8};
    bool plain = !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC));
    if (plain && BlockBytes(format) && levels != 1 && (w > h ? w : h) >= g_skip_min_size) {
        UINT skip = UINT(g_skip_mips);
        // Keep at least one level and 4x4 blocks on the host.
        while (skip && ((levels && skip >= levels) || (w >> skip) < 4 || (h >> skip) < 4)) --skip;
        a.skip = skip;
    }
    a.decode = plain && g_decode_dxt && IsDxt(format);
    if (a.decode && g_decode_16bit && format == D3DFMT_DXT1) a.decode_format = D3DFMT_A1R5G5B5;
    // Statistics: GPU memory of the top level as the guest asked for it and as created.
    static uint64_t count = 0, reduced = 0, decoded = 0, guest_bytes = 0, host_bytes = 0;
    UINT bb = BlockBytes(format);
    if (bb) {
        uint64_t px = uint64_t(w) * h;
        uint64_t g = IsDxt(format) ? px * bb / 16 : px * bb;
        uint64_t hpx = px >> (2 * a.skip);
        guest_bytes += g;
        host_bytes += a.decode ? hpx * (a.decode_format == D3DFMT_A1R5G5B5 ? 2 : 4) : g >> (2 * a.skip);
    }
    ++count;
    reduced += a.skip != 0;
    decoded += a.decode;
    if ((count & 1023) == 0) {
        Log("textures: %llu created, %llu reduced, %llu decoded; top levels %llu MiB -> %llu MiB",
            (unsigned long long)count, (unsigned long long)reduced, (unsigned long long)decoded,
            (unsigned long long)(guest_bytes >> 20), (unsigned long long)(host_bytes >> 20));
    }
    return a;
}

// ---- DXT decoding into A8R8G8B8 (memory order B, G, R, A) ----
void Rgb565(uint16_t c, uint8_t out[4]) {
    out[2] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[0] = uint8_t((c & 31) * 255 / 31);
    out[3] = 255;
}

// Colour block (DXT1 semantics when `dxt1`, else always four colours) -> 16 BGRA pixels.
void DecodeColorBlock(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int k = 0; k < 4; ++k) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
    } else {
        for (int k = 0; k < 4; ++k) pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;  // transparent black
    }
    uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}

void DecodeBlock(D3DFORMAT f, const uint8_t* b, uint8_t px[16][4]) {
    if (f == D3DFMT_DXT1) {
        DecodeColorBlock(b, true, px);
    } else if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {
        DecodeColorBlock(b + 8, false, px);
        for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    } else {  // DXT4/5: interpolated alpha
        DecodeColorBlock(b + 8, false, px);
        uint8_t a[8] = {b[0], b[1]};
        if (a[0] > a[1]) {
            for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1] + 3) / 7);
        } else {
            for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1] + 2) / 5);
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
    }
}

// Guest DXT data (`rows` block rows of `pitch` bytes) -> host BGRA pixels of a w x h region.
void DecodeDxt(D3DFORMAT f, const uint8_t* src, UINT pitch, UINT w, UINT h, uint8_t* dst, INT dst_pitch,
               D3DFORMAT dst_format) {
    const bool packed16 = dst_format == D3DFMT_A1R5G5B5;
    UINT bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = BlockBytes(f);
    uint8_t px[16][4];
    for (UINT by = 0; by < bh; ++by) {
        for (UINT bx = 0; bx < bw; ++bx) {
            DecodeBlock(f, src + by * pitch + bx * bytes, px);
            for (UINT y = 0; y < 4 && by * 4 + y < h; ++y) {
                for (UINT x = 0; x < 4 && bx * 4 + x < w; ++x) {
                    const uint8_t* p = px[y * 4 + x];
                    if (packed16) {  // B, G, R, A -> A1R5G5B5
                        uint16_t v = uint16_t((p[3] >= 128 ? 0x8000 : 0) | (p[2] >> 3) << 10 | (p[1] >> 3) << 5 |
                                              (p[0] >> 3));
                        memcpy(dst + INT(by * 4 + y) * dst_pitch + (bx * 4 + x) * 2, &v, 2);
                    } else {
                        memcpy(dst + INT(by * 4 + y) * dst_pitch + (bx * 4 + x) * 4, p, 4);
                    }
                }
            }
        }
    }
}

// ---- locks of adapted textures ----
struct AdaptedLock {
    uint32_t guest = 0, capacity = 0;  // guest buffer in the guest format
    UINT pitch = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN, decode_format = D3DFMT_A8R8G8B8;
    UINT width = 0, height = 0;         // region being written
    uint8_t* host = nullptr;            // locked host memory (decode target), null for dropped levels
    INT host_pitch = 0;
};
std::map<std::pair<void*, uint32_t>, AdaptedLock> g_adapted_locks;

// Locks guest level `level` of an adapted texture whose host level is locked through `lock_host`
// (HRESULT(UINT host_level, D3DLOCKED_RECT*)); writes D3DLOCKED_RECT to the guest.
template <typename LockHost>
HRESULT AdaptedLockRect(void* tex, uint32_t sub, const TexAdapt& a, UINT level, const RECT* rect, DWORD flags,
                        uint32_t out, LockHost lock_host) {
    UINT w = a.width >> level ? a.width >> level : 1, h = a.height >> level ? a.height >> level : 1;
    UINT pitch, rows;
    GuestLevelLayout(a.format, w, h, rect, &pitch, &rows);
    D3DLOCKED_RECT lr{};
    bool dropped = level < a.skip;
    if (!dropped) {
        HRESULT hr = lock_host(level - a.skip, &lr);
        if (FAILED(hr)) return hr;
        if (!a.decode) {  // same format and size on the host: plain shadow
            W32(out, uint32_t(lr.Pitch));
            W32(out + 4, ShadowLock(tex, sub, lr.pBits, uint32_t(lr.Pitch) * rows, flags));
            return D3D_OK;
        }
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    AdaptedLock& l = g_adapted_locks[{tex, sub}];
    uint32_t size = pitch * rows;
    if (l.capacity < size) {
        if (l.guest) HeapFree(l.guest);
        l.guest = HeapAlloc(size);
        l.capacity = size;
    }
    l.pitch = pitch;
    l.format = a.format;
    l.decode_format = a.decode_format;
    l.width = rect ? UINT(rect->right - rect->left) : w;
    l.height = rect ? UINT(rect->bottom - rect->top) : h;
    l.host = dropped ? nullptr : static_cast<uint8_t*>(lr.pBits);
    l.host_pitch = lr.Pitch;
    W32(out, pitch);
    W32(out + 4, l.guest);
    return D3D_OK;
}

// Returns true when the lock was a decoded or dropped one (the host level, if any, must still be unlocked).
bool AdaptedUnlock(void* tex, uint32_t sub, bool* host_locked) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto it = g_adapted_locks.find({tex, sub});
    if (it == g_adapted_locks.end() || !it->second.guest) return false;
    AdaptedLock& l = it->second;
    *host_locked = l.host != nullptr;
    if (l.host) {
        std::shared_lock<std::shared_mutex> copying(g_copy_rw);  // a release waits for this write (see g_copy_rw)
        DecodeDxt(l.format, Ptr<uint8_t>(l.guest), l.pitch, l.width, l.height, l.host, l.host_pitch, l.decode_format);
        l.host = nullptr;
    }
    return true;
}

void ForgetResource(void* host) {
    ForgetShadows(host);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_adapt.erase(host);
    for (auto it = g_adapted_locks.lower_bound({host, 0}); it != g_adapted_locks.end() && it->first.first == host;) {
        if (it->second.guest) HeapFree(it->second.guest);
        it = g_adapted_locks.erase(it);
    }
}

// Guest view of a level description: guest size and format.
void AdaptDesc(const TexAdapt& a, UINT level, D3DSURFACE_DESC* d) {
    d->Format = a.format;
    d->Width = a.width >> level ? a.width >> level : 1;
    d->Height = a.height >> level ? a.height >> level : 1;
}


// ---- display modes ----
// With override_modes (default on Android) the engine sees modes of the render resolution instead of the screen's:
// the render size (render_height) first, then 540p/720p/900p/1080p at the screen's aspect ratio. Its default
// fullscreen choice, the back buffer and the window client area then agree, and touch coordinates map 1:1.
bool OverrideModes() {
#ifdef __ANDROID__
    static const bool on = SettingInt("override_modes", 1) != 0;
#else
    static const bool on = SettingInt("override_modes", 0) != 0;
#endif
    return on;
}

std::vector<D3DDISPLAYMODE> Modes() {
    static std::vector<D3DDISPLAYMODE> modes;
    if (modes.empty()) {
        int cw, ch;
        ClientSize(&cw, &ch);
        modes.push_back({UINT(cw), UINT(ch), 60, D3DFMT_X8R8G8B8});
        for (UINT h : {540u, 720u, 900u, 1080u}) {
            UINT w = (UINT(uint64_t(h) * UINT(cw) / UINT(ch)) + 4) & ~7u;
            if (h != UINT(ch)) modes.push_back({w, h, 60, D3DFMT_X8R8G8B8});
        }
    }
    return modes;
}

void WriteMode(uint32_t out, const D3DDISPLAYMODE& m) {
    W32(out, m.Width);
    W32(out + 4, m.Height);
    W32(out + 8, m.RefreshRate);
    W32(out + 12, uint32_t(m.Format));
}

// ---- Direct3DCreate9 ----
void x_Direct3DCreate9(Ctx* c) {
    RunSelfTest();
    IDirect3D9* d3d = Direct3DCreate9(Arg(c, 0));
    Log("Direct3DCreate9(%u) -> %p", Arg(c, 0), static_cast<void*>(d3d));
    // GPUs without BC formats (Mali, PowerVR) get DXT textures decoded on the CPU; decode_dxt=1/0 forces it.
    if (d3d) {
        bool bc = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                                   D3DFMT_DXT5));
        g_decode_dxt = SettingInt("decode_dxt", bc ? 0 : 1) != 0;
        bool a1r5 = SUCCEEDED(d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                                     D3DFMT_A1R5G5B5));
#ifdef __ANDROID__
        // Off: with DXT1 as A1R5G5B5 parts of the character models vanish and the world loses its detail.
        g_decode_16bit = a1r5 && SettingInt("decode_16bit", 0) != 0;
#else
        g_decode_16bit = a1r5 && SettingInt("decode_16bit", 0) != 0;
#endif
        Log("GPU BC (DXT) textures: %s, decoding %s%s", bc ? "supported" : "not supported", g_decode_dxt ? "on" : "off",
            g_decode_dxt && g_decode_16bit ? " (DXT1 as 16-bit)" : "");
    }
    Return(c, Wrap(d3d), 1);
}

}  // namespace

// ---------------- manual methods ----------------
#define M(iface, method) void Manual_##iface##_##method(Ctx* c)

M(IDirect3D9, GetAdapterMonitor) { Return(c, 1, 2); }
M(IDirect3D9, GetAdapterDisplayMode) {
    // (this, Adapter, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3D9>(Arg(c, 0))->GetAdapterDisplayMode(Arg(c, 1), Ptr<D3DDISPLAYMODE>(Arg(c, 2)))),
               3);
        return;
    }
    WriteMode(Arg(c, 2), Modes()[0]);
    Return(c, D3D_OK, 3);
}
M(IDirect3D9, GetAdapterModeCount) {
    // (this, Adapter, Format)
    if (!OverrideModes()) {
        Return(c, Host<IDirect3D9>(Arg(c, 0))->GetAdapterModeCount(Arg(c, 1), D3DFORMAT(Arg(c, 2))), 3);
        return;
    }
    D3DFORMAT f = D3DFORMAT(Arg(c, 2));
    Return(c, f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 ? uint32_t(Modes().size()) : 0u, 3);
}
M(IDirect3D9, EnumAdapterModes) {
    // (this, Adapter, Format, Mode, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3D9>(Arg(c, 0))->EnumAdapterModes(Arg(c, 1), D3DFORMAT(Arg(c, 2)), Arg(c, 3),
                                                                        Ptr<D3DDISPLAYMODE>(Arg(c, 4)))),
               5);
        return;
    }
    auto modes = Modes();
    if (Arg(c, 3) >= modes.size()) {
        Return(c, uint32_t(D3DERR_INVALIDCALL), 5);
        return;
    }
    D3DDISPLAYMODE m = modes[Arg(c, 3)];
    m.Format = D3DFORMAT(Arg(c, 2));
    WriteMode(Arg(c, 4), m);
    Return(c, D3D_OK, 5);
}
M(IDirect3DDevice9, GetDisplayMode) {
    // (this, iSwapChain, pMode)
    if (!OverrideModes()) {
        Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->GetDisplayMode(Arg(c, 1),
                                                                            Ptr<D3DDISPLAYMODE>(Arg(c, 2)))),
               3);
        return;
    }
    WriteMode(Arg(c, 2), Modes()[0]);
    Return(c, D3D_OK, 3);
}

bool WaitForWindow(int seconds);  // below, with the surface state

M(IDirect3D9, CreateDevice) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 5));
    IDirect3DDevice9* dev = nullptr;
    // The Vulkan surface needs the window on screen: if the player switched to another app during startup, the
    // device would fail with a lost surface. Wait for the window, and retry a failure that happened while it was gone.
    HRESULT hr = D3DERR_INVALIDCALL;
    for (int attempt = 0; attempt < 5; ++attempt) {
        WaitForWindow(600);
        D3DPRESENT_PARAMETERS try_pp = pp;
        hr = Host<IDirect3D9>(Arg(c, 0))->CreateDevice(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)), HostHwnd(Arg(c, 3)),
                                                     Arg(c, 4), &try_pp, &dev);
        if (SUCCEEDED(hr)) {
            pp = try_pp;
            break;
        }
        if (!WaitForWindow(0)) {
            Log("CreateDevice -> %08x while the window was away, retrying", uint32_t(hr));
            continue;
        }
        break;  // failed with the window on screen: a real driver problem
    }
    Log("CreateDevice %ux%u fmt %u windowed %u msaa %u/%u interval %u -> %08x", pp.BackBufferWidth,
        pp.BackBufferHeight, pp.BackBufferFormat, pp.Windowed, pp.MultiSampleType, pp.MultiSampleQuality,
        pp.PresentationInterval, uint32_t(hr));
    // The engine does not check: without a device it crashes on the next call. The DXVK log says why it failed.
    if (FAILED(hr) && pp.MultiSampleType != D3DMULTISAMPLE_NONE) {
        // The engine probes multisampling by creating test devices until one fails: that is an answer, not an error.
        W32(Arg(c, 6), 0);
        Return(c, uint32_t(hr), 7);
        return;
    }
    if (FAILED(hr)) {
        StartupFailure("The graphics driver could not create the D3D9 device (DXVK on Vulkan). "
                       "See app_process64_d3d9.log for the reason.");
    }
    if (OverrideModes()) SetClientSize(int(pp.BackBufferWidth), int(pp.BackBufferHeight));
    ToGuest(pp, Arg(c, 5));
    W32(Arg(c, 6), Wrap(dev));
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DDevice9, GetCreationParameters) {
    D3DDEVICE_CREATION_PARAMETERS p;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->GetCreationParameters(&p);
    uint32_t g = Arg(c, 1);
    W32(g, p.AdapterOrdinal);
    W32(g + 4, p.DeviceType);
    W32(g + 8, HostWindow());
    W32(g + 12, p.BehaviorFlags);
    Return(c, uint32_t(hr), 2);
}
M(IDirect3DDevice9, SetCursorProperties) {
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetCursorProperties(
                  Arg(c, 1), Arg(c, 2), Unwrap<IDirect3DSurface9>(Arg(c, 3)))),
           4);
}
M(IDirect3DDevice9, CreateAdditionalSwapChain) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    IDirect3DSwapChain9* sc = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateAdditionalSwapChain(&pp, &sc);
    ToGuest(pp, Arg(c, 1));
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(sc) : 0);
    Return(c, uint32_t(hr), 3);
}
#ifdef MAFIA2RE_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
#endif
void LazyDsReset();
void LazyDsPoll(IDirect3DDevice9* dev);
void AbPoll(uint64_t frame);
void EnvFrameReset();
extern int g_ab;
M(IDirect3DDevice9, Reset) {
    D3DPRESENT_PARAMETERS pp = ToHost(Arg(c, 1));
    DrainGraves(true);
    LazyDsReset();
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->Reset(&pp);
    Log("Reset %ux%u msaa %u/%u interval %u -> %08x", pp.BackBufferWidth, pp.BackBufferHeight, pp.MultiSampleType,
        pp.MultiSampleQuality, pp.PresentationInterval, uint32_t(hr));
    if (SUCCEEDED(hr) && OverrideModes()) SetClientSize(int(pp.BackBufferWidth), int(pp.BackBufferHeight));
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}
// Setting shot=<frame>[,<frame>...] (MAFIA2RE_SHOT): the back buffer of those presents is saved as shot_<frame>.ppm in
// the working directory (debugging without a desktop screenshot tool; on Android only the game's own frame).
// Writes the back buffer as a PPM (shared by the screenshot options and the debug console's `drawshot`).
bool SaveBackbuffer(IDirect3DDevice9* dev, const std::string& name) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    D3DLOCKED_RECT lr;
    bool ok = false;
    if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (FILE* f = fopen(name.c_str(), "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            std::vector<uint8_t> row(3 * d.Width);
            for (uint32_t y = 0; y < d.Height; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * size_t(lr.Pitch);
                for (uint32_t x = 0; x < d.Width; ++x) {
                    row[3 * x] = src[4 * x + 2];
                    row[3 * x + 1] = src[4 * x + 1];
                    row[3 * x + 2] = src[4 * x];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            ok = true;
        }
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    bb->Release();
    return ok;
}
// Debug console `drawshot N`: save the frame as it is right after the N-th draw call of a frame (0-based).
std::atomic<int> g_draw_index{0}, g_shot_after_draw{-1};
void DebugAfterDraw(void* dev) {
    int n = g_draw_index.fetch_add(1);
    if (n == g_shot_after_draw.load()) {
        g_shot_after_draw = -1;
        SaveBackbuffer(static_cast<IDirect3DDevice9*>(dev), "shot_draw.ppm");
    }
}
void DebugDrawShot(int n) {
    unlink("shot_draw.ppm");
    g_shot_after_draw = n;
}
void MaybeScreenshot(IDirect3DDevice9* dev) {
    g_draw_index = 0;
    static const std::string spec_setting = Setting("shot", "");  // mafia2re.ini `shot = ...` works on Android
    static const char* spec = spec_setting.empty() ? nullptr : spec_setting.c_str();
    static uint32_t frame = 0;
    ++frame;
    // On demand: a file named shot_now in the working directory (adb shell touch ...) asks for one frame.
    bool now = (frame & 15) == 0 && access("shot_now", F_OK) == 0;
    if (now) unlink("shot_now");
    if (!spec && !now) return;
    bool want = now;
    for (const char* p = spec ? spec : ""; *p;) {
        char* end;
        if (strtoul(p, &end, 10) == frame) want = true;
        p = *end ? end + 1 : end;
    }
    if (!want) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    D3DLOCKED_RECT lr;
    if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::string name = now ? std::string("shot_now.ppm") : "shot_" + std::to_string(frame) + ".ppm";
        if (FILE* f = fopen(name.c_str(), "wb")) {
            fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            std::vector<uint8_t> row(3 * d.Width);
            for (uint32_t y = 0; y < d.Height; ++y) {
                const uint8_t* src = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * size_t(lr.Pitch);
                for (uint32_t x = 0; x < d.Width; ++x) {
                    row[3 * x] = src[4 * x + 2];
                    row[3 * x + 1] = src[4 * x + 1];
                    row[3 * x + 2] = src[4 * x];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
            Log("screenshot %s (%ux%u)", name.c_str(), d.Width, d.Height);
        }
        sys->UnlockRect();
    } else {
        Log("screenshot of frame %u failed (format %d)", frame, int(d.Format));
    }
    if (sys) sys->Release();
    bb->Release();
}

// The window can go away under the render thread (Android destroys the surface when the app leaves the screen):
// GameSurface.surfaceDestroyed waits for a present in flight and holds presents back until the surface is back,
// as presenting or recreating the swap chain on a destroyed window crashes in the Vulkan loader. A skipped present
// just drops the frame.
std::mutex g_present_mutex;
bool g_surface_gone = false;  // under g_present_mutex

template <typename F>
HRESULT GuardedPresent(F present) {
    std::unique_lock<std::mutex> lock(g_present_mutex);
    if (g_surface_gone) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return D3D_OK;
    }
    return present();
}

#ifdef __ANDROID__
// Waits up to `seconds` for the game's window (surface) to be on screen; true if it is.
bool WaitForWindow(int seconds) {
    for (int i = 0; i <= seconds * 10; ++i) {
        {
            std::lock_guard<std::mutex> lock(g_present_mutex);
            if (!g_surface_gone) return true;
        }
        if (i < seconds * 10) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

extern "C" JNIEXPORT jfloat JNICALL Java_com_mafia2re_game_TouchControls_nativeFps(JNIEnv*, jclass) {
    return native::g_fps.load(std::memory_order_relaxed);
}
extern "C" JNIEXPORT jfloat JNICALL Java_com_mafia2re_game_TouchControls_nativeFrameMs(JNIEnv*, jclass) {
    float fps = native::g_fps.load(std::memory_order_relaxed);
    return fps > 0 ? 1000.0f / fps : 0;
}
extern "C" JNIEXPORT jint JNICALL Java_com_mafia2re_game_TouchControls_nativeMemoryMiB(JNIEnv*, jclass) {
    long pages = 0, resident = 0;
    if (FILE* f = fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    return jint(resident * (sysconf(_SC_PAGESIZE) / 1024) / 1024);
}
extern "C" JNIEXPORT void JNICALL Java_com_mafia2re_game_TouchControls_nativeSetFpsLimit(JNIEnv*, jclass, jint) {}
extern "C" JNIEXPORT void JNICALL Java_com_mafia2re_game_GameSurface_nativeSurfaceGone(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_surface_gone = true;
}
extern "C" JNIEXPORT void JNICALL Java_com_mafia2re_game_GameSurface_nativeSurfaceReady(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(g_present_mutex);
    g_surface_gone = false;
}
#else
// Desktop: the window stays on screen.
bool WaitForWindow(int) { return true; }
#endif

// Battlefront II waits for the GPU at the end of a frame by calling GetData on an event query in a tight loop (millions of
// calls a second on the desktop). The answer is the same, but polling costs a whole core and battery on a phone:
// only the first miss of a streak flushes (D3DGETDATA_FLUSH makes DXVK submit), later misses yield the core and,
// for a longer wait, sleep a little.
// fake_gpu = <vendor hex>:<device hex>:<description> (MAFIA2RE_FAKE_GPU): report another adapter to the game, to
// reproduce on the desktop what the game does with a phone GPU (its configuration scripts pick settings by vendor
// and device id).
M(IDirect3D9, GetAdapterIdentifier) {
    auto* id = reinterpret_cast<D3DADAPTER_IDENTIFIER9*>(Ptr<void>(Arg(c, 3)));
    HRESULT r = Host<IDirect3D9>(Arg(c, 0))->GetAdapterIdentifier(Arg(c, 1), Arg(c, 2), id);
    static const std::string fake = Setting("fake_gpu", "");
    if (SUCCEEDED(r) && !fake.empty()) {
        unsigned vendor = 0, device = 0;
        char desc[256] = {0};
        if (sscanf(fake.c_str(), "%x:%x:%255[^\n]", &vendor, &device, desc) >= 2) {
            id->VendorId = vendor;
            id->DeviceId = device;
            if (desc[0]) snprintf(id->Description, sizeof id->Description, "%s", desc);
        }
    }
    // The game's configuration scripts only know AMD, NVIDIA, Intel and S3 adapters (Config/VideoCards.txt): for any
    // other vendor (phone GPUs: ARM Mali, Qualcomm Adreno, Imagination) the script stops with "Unknown variable:
    // 'isCardFound'". Such adapters are reported as a mid-range card from that table, a Radeon HD 2600, so the game
    // finds a card and picks moderate settings.
    if (SUCCEEDED(r)) {
        // An explicit gpu_tier applies to any adapter (also known ones, e.g. to try the low settings on a PC).
        const bool forced = !Setting("gpu_tier", "").empty();
        switch (forced ? 0u : id->VendorId) {
            case 0x1002: case 0x10de: case 0x10b4: case 0x12d2: case 0x8086: case 0x5333: break;
            default: {
                // The card the game sees picks its graphics settings. Setting gpu_tier (the settings panel's "Game
                // graphics"): 0 = Radeon X1300 (low), 1 = Radeon HD 2600 (medium, default), 2 = Radeon HD 4850 (high).
                static const struct { uint32_t device; const char* name; } kTiers[] = {
                    {0x7142, "ATI Radeon X1300"}, {0x9588, "ATI Radeon HD 2600"}, {0x9442, "ATI Radeon HD 4850"}};
                int tier = SettingInt("gpu_tier", 1);
                if (tier < 0 || tier > 2) tier = 1;
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    Log("d3d9: adapter \"%s\" (vendor %04x) is unknown to the game; reported as %s",
                        id->Description, unsigned(id->VendorId), kTiers[tier].name);
                }
                id->VendorId = 0x1002;
                id->DeviceId = kTiers[tier].device;
                snprintf(id->Description, sizeof id->Description, "%s", kTiers[tier].name);
            }
        }
    }
    if (SUCCEEDED(r)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("d3d9: adapter \"%s\" vendor %04x device %04x driver %s", id->Description, unsigned(id->VendorId),
                unsigned(id->DeviceId), id->Driver);
        }
    }
    Return(c, uint32_t(r), 4);
}

bool SkipOcclusionPass();
extern bool g_rt_is_occlusion;
// Render variants switchable in one scene (AbPoll below): the one named in the file ab_off is active.
enum AbMode { kAbNone, kAbAniso, kAbPrepass, kAbPost, kAbShadows, kAbEnv, kAbOccWait };
extern int g_ab;
extern std::atomic<uint32_t> g_occ_pending, g_occ_pending_total, g_env_skipped, g_shadow_skipped;
// Occlusion queries. Only those begun while the dropped occlusion pass is bound get a made-up answer ("visible");
// the others (light coronas test their light's position against the depth buffer this way) keep their real result.
// With query_nowait, a result that is not ready yet is answered with that query's previous result, not "visible":
// "always visible" drew every lamp's glow through walls.
struct QueryState {
    bool faked = false;
    uint64_t miss_frame = ~0ull;  // the frame of the last "not ready", and how many in that frame (spin detection)
    uint32_t misses_this_frame = 0;
};
std::mutex g_query_mutex;
std::unordered_map<IDirect3DQuery9*, QueryState> g_queries;
void QueryIssued(IDirect3DQuery9* q, uint32_t flags) {
    if (!(flags & D3DISSUE_BEGIN) || q->GetType() != D3DQUERYTYPE_OCCLUSION) return;
    std::lock_guard<std::mutex> lock(g_query_mutex);
    g_queries[q].faked = g_rt_is_occlusion && SkipOcclusionPass();
}
M(IDirect3DQuery9, GetData) {
    static thread_local uint32_t misses = 0;
#ifdef __ANDROID__
    static const bool nowait = SettingInt("query_nowait", 1) != 0;
#else
    static const bool nowait = SettingInt("query_nowait", 0) != 0;
#endif
    auto* q = Host<IDirect3DQuery9>(Arg(c, 0));
    DWORD flags = Arg(c, 3);
    // A flush on the first miss is usually enough; if the event was not yet submitted then (e.g. right after a device
    // reset), DXVK would never submit it while the game spins here, so flush again now and then.
    if (misses > 0 && (misses & 63) != 0) flags &= ~D3DGETDATA_FLUSH;
    const D3DQUERYTYPE type = q->GetType();
    QueryState* st = nullptr;
    if (type == D3DQUERYTYPE_OCCLUSION) {
        std::lock_guard<std::mutex> lock(g_query_mutex);
        st = &g_queries[q];  // node-based map: the pointer stays valid
        if (st->faked) {
            if (Arg(c, 1) && Arg(c, 2) >= 4) W32(Arg(c, 1), 1000u);
            Return(c, 0, 4);
            return;
        }
    }
    // Occlusion queries answered honestly (setting query_occlusion_honest, or the variant "occwait"): "not
    // ready" goes back to the game, which then keeps its last visibility, as on a PC; the answer "visible" drew lamp
    // coronas through walls.
    // On by default on Android: the engine polls each query about once a frame and moves on when it is not ready (no
    // spinning), so "not ready" costs nothing and the coronas stay behind walls.
#ifdef __ANDROID__
    static const bool occ_honest_setting = SettingInt("query_occlusion_honest", 1) != 0;
#else
    static const bool occ_honest_setting = SettingInt("query_occlusion_honest", 0) != 0;
#endif
    const bool occ_honest = type == D3DQUERYTYPE_OCCLUSION && (occ_honest_setting || g_ab == kAbOccWait);
    const bool wait_free = nowait && !occ_honest;
    // A query that already said "not ready" this frame says it again without asking DXVK: the game polls each one
    // many times a frame, and every ask is a DXVK call. It may
    // become ready a frame later than it would have.
    static const bool repoll_cache = SettingInt("query_repoll_cache", 1) != 0;
    if (occ_honest && st && repoll_cache) {
        std::lock_guard<std::mutex> lock(g_query_mutex);
        const uint64_t frame = g_frames.load(std::memory_order_relaxed);
        if (st->miss_frame == frame && st->misses_this_frame < 64) {
            ++st->misses_this_frame;
            g_occ_pending.fetch_add(1, std::memory_order_relaxed);
            g_occ_pending_total.fetch_add(1, std::memory_order_relaxed);
            ++misses;
            Return(c, uint32_t(S_FALSE), 4);
            return;
        }
    }
    HRESULT r = q->GetData(Ptr<void>(Arg(c, 1)), Arg(c, 2), wait_free ? 0 : flags);
    if (r == S_FALSE && occ_honest) { g_occ_pending.fetch_add(1, std::memory_order_relaxed); g_occ_pending_total.fetch_add(1, std::memory_order_relaxed); }
    // Some engine code (f_017ab360, in cutscenes and while streaming) spins on an occlusion query until it is ready,
    // and on Mali that took 2-5 s a frame (render thread asleep in this loop, GPU idle). A query asked 64 times in one
    // frame is being spun on, not polled once a frame: answer "visible" so the frame goes on.
    bool spinning = false;
    if (r == S_FALSE && occ_honest && st) {
        std::lock_guard<std::mutex> lock(g_query_mutex);
        const uint64_t frame = g_frames.load(std::memory_order_relaxed);
        if (st->miss_frame == frame) ++st->misses_this_frame;
        else st->miss_frame = frame, st->misses_this_frame = 1;
        spinning = st->misses_this_frame >= 64;
    }
    if (spinning && Arg(c, 1) && Arg(c, 2) >= 4) {
        W32(Arg(c, 1), 1000u);
        r = S_OK;
        static std::atomic<uint32_t> spun{0};
        if (spun.fetch_add(1) < 5) Log("d3d9: occlusion query spun on, answered visible");
    }
    if (r == S_FALSE && wait_free) {
        // The game waits on these every frame, and a GPU round trip on a phone's driver costs far more than on the
        // desktop: the render thread sat here 92% of the time with the GPU idle. Answer at once: an event has
        // "passed", an occlusion query saw pixels. (Repeating the query's previous answer instead made the player's car
        // body vanish: the game reuses query objects for other things.)
        if ((type == D3DQUERYTYPE_EVENT || type == D3DQUERYTYPE_OCCLUSION) && Arg(c, 1) && Arg(c, 2) >= 4) {
            W32(Arg(c, 1), type == D3DQUERYTYPE_EVENT ? 1u : 1000u);
            r = S_OK;
        }
    }
    if (r == S_FALSE) {
        ++misses;
        if (misses > 256) std::this_thread::sleep_for(std::chrono::microseconds(100));
        else if (misses > 16) std::this_thread::yield();
    } else {
        misses = 0;
    }
    Return(c, uint32_t(r), 4);
}

void RtLog();
void ApiLog(uint64_t frame);
void GpuFrame(IDirect3DDevice9* dev, uint64_t frame);
void ShaderPrewarmStep(IDirect3DDevice9* dev);  // below: shaders from shader_cache.bin, a few a frame
M(IDirect3DDevice9, Present) {
    ++g_present_count;
    ShaderPrewarmStep(Host<IDirect3DDevice9>(Arg(c, 0)));
#ifdef __ANDROID__
    // Mafia II renders on its own thread, which is the busiest one: it belongs on the big cores and in the hint session.
    static thread_local bool joined = false;
    if (!joined) {
        joined = true;
        ::PinToBigCores();
        PerfHintAddThread(int(gettid()));
    }
#endif
    RtLog();
    uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;  // the watchdog's frame counter (main.cpp)
    ApiLog(frame);
    // ADPF: how long this frame's work took (from the end of the previous Present to now, before any pacing sleep).
    {
        static auto work_start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        PerfHintFrame(std::chrono::duration_cast<std::chrono::nanoseconds>(now - work_start).count());
        work_start = now;
    }
    // fps_limit (mafia2re.ini, the settings panel's presets): frames are paced to at most that rate, which keeps a
    // phone cooler (and its clocks up longer) when the game would otherwise run unevenly above it.
    {
        static const int limit = SettingInt("fps_limit", 0);
        static auto next = std::chrono::steady_clock::now();
        if (limit > 0) {
            auto now = std::chrono::steady_clock::now();
            if (now < next) std::this_thread::sleep_until(next);
            next = std::max(now, next) + std::chrono::microseconds(1000000 / limit);
        }
    }
    // FPS over the last second, for the on-screen counter (TouchControls.nativeFps).
    {
        static auto window = std::chrono::steady_clock::now();
        static uint64_t window_frame = 0;
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - window).count();
        if (secs >= 1.0) {
            g_fps.store(float(double(frame - window_frame) / secs), std::memory_order_relaxed);
            window = now;
            window_frame = frame;
        }
    }
    // A perf line every 10 s: frames per second and the frame count it was measured at.
    {
        static auto last = std::chrono::steady_clock::now();
        static uint64_t last_frame = 0;
        auto now = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - last).count();
        // Hitches the average hides: the longest frame and the frames over 100 ms in this window. A stall is logged
        // when it happens (with the frame number), so a freeze in a cutscene shows where it was.
        static auto prev_present = now;
        static double worst_ms = 0;
        static unsigned slow = 0;
        const double frame_ms = std::chrono::duration<double, std::milli>(now - prev_present).count();
        prev_present = now;
        if (frame_ms > worst_ms) worst_ms = frame_ms;
        if (frame_ms > 100) ++slow;
        if (frame_ms > 500) Log("perf: stall %.0f ms at frame %llu", frame_ms, (unsigned long long)frame);
        AutoplayFrame(frame_ms);
        {   // per-frame deltas; a frame over 50 ms gets a line (at most 20 per 10 s)
            static uint32_t last_sh = 0, last_tex = 0, hitch_lines = 0;
            static uint64_t last_copied = 0, last_read = 0;
            const uint32_t sh = g_hitch_shaders.load(std::memory_order_relaxed), tex = g_hitch_textures.load(std::memory_order_relaxed);
            const uint64_t copied = g_hitch_copied.load(std::memory_order_relaxed), rd = g_io_total_bytes.load(std::memory_order_relaxed);
            if (frame_ms > 50 && frame > 300 && hitch_lines < 20) {
                ++hitch_lines;
                Log("perf: hitch %.0f ms at frame %llu: %u draws (%u in shadow maps), %u shaders, %u textures created, %llu KB copied, %llu KB read",
                    frame_ms, (unsigned long long)frame, g_frame_draws, g_frame_shadow_draws, sh - last_sh, tex - last_tex,
                    (unsigned long long)((copied - last_copied) >> 10), (unsigned long long)((rd - last_read) >> 10));
            }
            if (secs >= 10.0) hitch_lines = 0;
            last_sh = sh; last_tex = tex; last_copied = copied; last_read = rd;
            if (frame_ms > 0 && frame % 300 == 0)
                Log("perf: frame %llu had %u draws (%u in shadow maps)", (unsigned long long)frame, g_frame_draws, g_frame_shadow_draws);
            g_frame_draws = g_frame_shadow_draws = 0;
        }
        if (frame == 300) VulkanDriverConfirm();  // 300 frames on the chosen driver: Turnip works on this phone
        if (secs >= 10.0) {
            Log("perf: longest frame %.0f ms, %u frames over 100 ms", worst_ms, slow);
            Log("perf: io %s", TakeIoStats().c_str());
            Log("perf: game created %u shaders, %.0f ms inside DXVK", g_shader_creates.exchange(0),
                double(g_shader_create_ns.exchange(0)) / 1e6);
            worst_ms = 0;
            slow = 0;
            // Is the frame CPU-bound? CPU time of this (the render) thread and of the whole process per frame: a
            // render thread near 100% busy means the GPU is not what limits the frame rate.
            static double last_thread = 0, last_proc = 0;
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            const double thread_s = double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
            const double proc_s = double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
            const double frames = double(frame - last_frame ? frame - last_frame : 1);
            Log("perf: render thread %.1f ms of CPU a frame (%.0f%% busy), all threads %.1f ms a frame (%.2f cores)",
                (thread_s - last_thread) * 1000.0 / frames, 100.0 * (thread_s - last_thread) / secs,
                (proc_s - last_proc) * 1000.0 / frames, (proc_s - last_proc) / secs);
            last_thread = thread_s;
            last_proc = proc_s;
            Log("perf: frame %llu, %.1f fps, lock copies %llu MB in %llu locks, %llu ms, occlusion not ready %u, cube-face draws dropped %u, shadow draws dropped %u", (unsigned long long)frame, double(frame - last_frame) / secs,
                (unsigned long long)(g_shadow_bytes.exchange(0) >> 20), (unsigned long long)g_shadow_calls.exchange(0), (unsigned long long)(g_shadow_ns.exchange(0) / 1000000),
                g_occ_pending_total.exchange(0), g_env_skipped.exchange(0), g_shadow_skipped.exchange(0));
            last = now;
            last_frame = frame;
        }
    }
    if (!g_graves.empty()) DrainGraves(false);
    auto* dev = Host<IDirect3DDevice9>(Arg(c, 0));
    LazyDsPoll(dev);
    AbPoll(frame);
    EnvFrameReset();
    ::PinDxvkThreads();
#ifdef MAFIA2RE_PGO_GEN
    {  // PGO=gen: the profile goes to files/mafia2.profraw every 60 s (an Android app is killed, never exits)
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(60)) {
            last = std::chrono::steady_clock::now();
            int r = __llvm_profile_write_file();
            Log("pgo: profile written (%d)", r);
        }
    }
#endif
    MaybeScreenshot(dev);
    GpuFrame(dev, frame);
    RECT bars;
    const RECT* dst = Ptr<RECT>(Arg(c, 2));
    int bx, by, bw, bh;
    if (!dst && PresentRect(&bx, &by, &bw, &bh)) {  // a 16:9 frame in a wider window (user32.cpp)
        bars = {bx, by, bx + bw, by + bh};
        dst = &bars;
    }
    HRESULT hr = GuardedPresent([&] { return dev->Present(Ptr<RECT>(Arg(c, 1)), dst, nullptr, nullptr); });
    Return(c, uint32_t(hr), 5);
}
// Sampler state passes through; the first 200000 calls are tallied once to log the filtering the engine asks for
// (anisotropy and mip filtering are what a phone GPU pays for).
M(IDirect3DDevice9, SetSamplerState) {
    static std::atomic<uint32_t> calls{0};
    static std::atomic<uint32_t> aniso[17], minf[4], mipf[4];
    const DWORD type = Arg(c, 2), value = Arg(c, 3);
    if (uint32_t n = calls.fetch_add(1); n < 200000) {
        if (type == D3DSAMP_MAXANISOTROPY) ++aniso[value > 16 ? 16 : value];
        if (type == D3DSAMP_MINFILTER) ++minf[value & 3];
        if (type == D3DSAMP_MIPFILTER) ++mipf[value & 3];
        if (n == 199999) {
            std::string a;
            for (int i = 0; i <= 16; ++i)
                if (aniso[i]) a += " " + std::to_string(i) + "x:" + std::to_string(aniso[i].load());
            Log("samplers: max anisotropy%s | min filter none/point/linear/aniso %u/%u/%u/%u | mip %u/%u/%u/%u",
                a.c_str(), minf[0].load(), minf[1].load(), minf[2].load(), minf[3].load(), mipf[0].load(),
                mipf[1].load(), mipf[2].load(), mipf[3].load());
        }
    }
    // max_aniso (default 16 = as the game asks): cap on anisotropic filtering, which the game sets to 16x on about
    // half its samplers; the variant "aniso" caps it at 2.
    static const DWORD max_aniso = DWORD(SettingInt("max_aniso", 16));
    DWORD v = value;
    if (type == D3DSAMP_MAXANISOTROPY) {
        DWORD cap = g_ab == 1 ? 2 : max_aniso;
        if (v > cap) v = cap;
    }
    Return(c, uint32_t(Host<IDirect3DDevice9>(Arg(c, 0))->SetSamplerState(Arg(c, 1), D3DSAMPLERSTATETYPE(type), v)), 4);
}
// The engine's worker threads poll this before every buffer lock; DXVK answers D3D_OK for a windowed device but
// takes the device lock first, so the polls queue up behind the render thread's draws and presents.
M(IDirect3DDevice9, TestCooperativeLevel) { Return(c, uint32_t(D3D_OK), 1); }
M(IDirect3DSwapChain9, Present) {
    auto* sc = Host<IDirect3DSwapChain9>(Arg(c, 0));
    HRESULT hr = GuardedPresent([&] { return sc->Present(Ptr<RECT>(Arg(c, 1)), Ptr<RECT>(Arg(c, 2)), nullptr, nullptr,
                                                         Arg(c, 5)); });
    Return(c, uint32_t(hr), 6);
}
M(IDirect3DSwapChain9, GetPresentParameters) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr = Host<IDirect3DSwapChain9>(Arg(c, 0))->GetPresentParameters(&pp);
    ToGuest(pp, Arg(c, 1));
    Return(c, uint32_t(hr), 2);
}

// GetContainer(riid, void**): the container is a texture or the device.
template <typename T>
void GetContainerImpl(Ctx* c) {
    const GUID& riid = *Ptr<GUID>(Arg(c, 1));
    void* p = nullptr;
    HRESULT hr = Host<T>(Arg(c, 0))->GetContainer(riid, &p);
    uint32_t g = 0;
    if (SUCCEEDED(hr)) {
        if (riid == __uuidof(IDirect3DTexture9)) g = Wrap(static_cast<IDirect3DTexture9*>(p));
        else if (riid == __uuidof(IDirect3DCubeTexture9)) g = Wrap(static_cast<IDirect3DCubeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DVolumeTexture9)) g = Wrap(static_cast<IDirect3DVolumeTexture9*>(p));
        else if (riid == __uuidof(IDirect3DBaseTexture9)) g = Wrap(static_cast<IDirect3DBaseTexture9*>(p));
        else if (riid == __uuidof(IDirect3DDevice9)) g = Wrap(static_cast<IDirect3DDevice9*>(p));
        else Log("d3d9: GetContainer for an unsupported interface");
    }
    W32(Arg(c, 2), g);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DSurface9, GetContainer) { GetContainerImpl<IDirect3DSurface9>(c); }
M(IDirect3DVolume9, GetContainer) { GetContainerImpl<IDirect3DVolume9>(c); }

M(IDirect3DSurface9, LockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    const RECT* rect = Ptr<RECT>(Arg(c, 2));
    D3DLOCKED_RECT lr;
    HRESULT hr = s->LockRect(&lr, rect, Arg(c, 3));
    D3DSURFACE_DESC desc;
    s->GetDesc(&desc);
    LockRectCommon(c, s, 0, hr, lr, desc, rect, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DSurface9, UnlockRect) {
    auto* s = Host<IDirect3DSurface9>(Arg(c, 0));
    ShadowUnlock(s, 0);
    Return(c, uint32_t(s->UnlockRect()), 1);
}
M(IDirect3DSurface9, GetDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }
M(IDirect3DSurface9, ReleaseDC) { Return(c, uint32_t(D3DERR_INVALIDCALL), 2); }

M(IDirect3DDevice9, CreateTexture) {
    g_hitch_textures.fetch_add(1, std::memory_order_relaxed);
    // (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
    UINT w = Arg(c, 1), h = Arg(c, 2), levels = Arg(c, 3);
    DWORD usage = Arg(c, 4);
    D3DFORMAT format = D3DFORMAT(Arg(c, 5));
    // Experiment (setting rt_r32f_format, a D3DFORMAT number): another format for R32F render targets.
    static const int r32f_as = SettingInt("rt_r32f_format", 0);
    if (r32f_as && (usage & D3DUSAGE_RENDERTARGET) && format == D3DFMT_R32F) format = D3DFORMAT(r32f_as);
    // The refraction copies (StretchRect of the A8R8G8B8 scene into an R5G6B5 render target, twice a frame): same
    // format as the source lets DXVK copy instead of a converting blit pass. Setting rt_r5g6b5_format (0 = keep).
    static const int r565_as = SettingInt("rt_r5g6b5_format", int(D3DFMT_A8R8G8B8));
    if (r565_as && (usage & D3DUSAGE_RENDERTARGET) && format == D3DFMT_R5G6B5) {
        Log("d3d9: R5G6B5 render target %ux%u created as format %d", w, h, r565_as);
        format = D3DFORMAT(r565_as);
    }
    TexAdapt a = PlanTexture(w, h, levels, usage, format);
    static const bool trace_tex = getenv("MAFIA2RE_TRACE_D3D") != nullptr;
    if (trace_tex) Log("CreateTexture %ux%u levels %u usage %x fmt %x pool %u (from %08x)", w, h, levels, usage, unsigned(format), Arg(c, 6), R32(c->esp));
    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateTexture(
        w >> a.skip, h >> a.skip, levels ? levels - a.skip : 0, usage, a.decode ? a.decode_format : format,
        D3DPOOL(Arg(c, 6)), &tex, nullptr);
    if (SUCCEEDED(hr) && (a.skip || a.decode)) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_adapt[tex] = a;
    }
    W32(Arg(c, 7), SUCCEEDED(hr) ? Wrap(tex) : 0);
    Return(c, uint32_t(hr), 9);
}
M(IDirect3DDevice9, CreateCubeTexture) {
    // (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
    UINT edge = Arg(c, 1), levels = Arg(c, 2);
    DWORD usage = Arg(c, 3);
    D3DFORMAT format = D3DFORMAT(Arg(c, 4));
    TexAdapt a = PlanTexture(edge, edge, levels, usage, format);
    IDirect3DCubeTexture9* tex = nullptr;
    HRESULT hr = Host<IDirect3DDevice9>(Arg(c, 0))->CreateCubeTexture(
        edge >> a.skip, levels ? levels - a.skip : 0, usage, a.decode ? a.decode_format : format, D3DPOOL(Arg(c, 5)),
        &tex, nullptr);
    if (SUCCEEDED(hr) && (a.skip || a.decode)) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_adapt[tex] = a;
    }
    W32(Arg(c, 6), SUCCEEDED(hr) ? Wrap(tex) : 0);
    Return(c, uint32_t(hr), 8);
}
M(IDirect3D9, CheckDeviceFormat) {
    // (this, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat): DXT is always available when decoded.
    D3DFORMAT check = D3DFORMAT(Arg(c, 6));
    HRESULT hr = Host<IDirect3D9>(Arg(c, 0))->CheckDeviceFormat(Arg(c, 1), D3DDEVTYPE(Arg(c, 2)),
                                                              D3DFORMAT(Arg(c, 3)), Arg(c, 4),
                                                              D3DRESOURCETYPE(Arg(c, 5)), check);
    if (FAILED(hr) && g_decode_dxt && IsDxt(check) && !(Arg(c, 4) & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) {
        hr = D3D_OK;
    }
    Return(c, uint32_t(hr), 7);
}
M(IDirect3DTexture9, GetLevelCount) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    Return(c, t->GetLevelCount() + (a ? a->skip : 0), 1);
}
M(IDirect3DCubeTexture9, GetLevelCount) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    Return(c, t->GetLevelCount() + (a ? a->skip : 0), 1);
}
M(IDirect3DTexture9, GetLevelDesc) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    UINT level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    D3DSURFACE_DESC* d = Ptr<D3DSURFACE_DESC>(Arg(c, 2));
    HRESULT hr = t->GetLevelDesc(a && level >= a->skip ? level - a->skip : a ? 0 : level, d);
    if (SUCCEEDED(hr) && a) AdaptDesc(*a, level, d);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DCubeTexture9, GetLevelDesc) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    UINT level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    D3DSURFACE_DESC* d = Ptr<D3DSURFACE_DESC>(Arg(c, 2));
    HRESULT hr = t->GetLevelDesc(a && level >= a->skip ? level - a->skip : a ? 0 : level, d);
    if (SUCCEEDED(hr) && a) AdaptDesc(*a, level, d);
    Return(c, uint32_t(hr), 3);
}
// Surfaces of adapted textures: the nearest host level (dropped levels map to the top host level).
M(IDirect3DTexture9, GetSurfaceLevel) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    UINT level = Arg(c, 1);
    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = t->GetSurfaceLevel(a ? (level >= a->skip ? level - a->skip : 0) : level, &surf);
    W32(Arg(c, 2), SUCCEEDED(hr) ? Wrap(surf) : 0);
    Return(c, uint32_t(hr), 3);
}
M(IDirect3DCubeTexture9, GetCubeMapSurface) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    const TexAdapt* a = AdaptOf(t);
    UINT level = Arg(c, 2);
    IDirect3DSurface9* surf = nullptr;
    HRESULT hr = t->GetCubeMapSurface(D3DCUBEMAP_FACES(Arg(c, 1)), a ? (level >= a->skip ? level - a->skip : 0) : level,
                                      &surf);
    W32(Arg(c, 3), SUCCEEDED(hr) ? Wrap(surf) : 0);
    Return(c, uint32_t(hr), 4);
}
M(IDirect3DTexture9, LockRect) {
    // (this, Level, pLockedRect, pRect, Flags)
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const RECT* rect = Ptr<RECT>(Arg(c, 3));
    if (const TexAdapt* a = AdaptOf(t)) {
        TexAdapt copy = *a;
        HRESULT hr = AdaptedLockRect(t, level, copy, level, rect, Arg(c, 4), Arg(c, 2),
                                     [&](UINT host_level, D3DLOCKED_RECT* lr) {
                                         return t->LockRect(host_level, lr, rect, Arg(c, 4));
                                     });
        Return(c, uint32_t(hr), 5);
        return;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(level, &lr, rect, Arg(c, 4));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, level, hr, lr, desc, rect, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DTexture9, UnlockRect) {
    auto* t = Host<IDirect3DTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const TexAdapt* a = AdaptOf(t);
    bool host_locked = true;
    if (a && AdaptedUnlock(t, level, &host_locked)) {
        Return(c, host_locked ? uint32_t(t->UnlockRect(level - a->skip)) : 0u, 2);
        return;
    }
    ShadowUnlock(t, level);
    Return(c, uint32_t(t->UnlockRect(a ? level - a->skip : level)), 2);
}
M(IDirect3DCubeTexture9, LockRect) {
    // (this, FaceType, Level, pLockedRect, pRect, Flags)
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const RECT* rect = Ptr<RECT>(Arg(c, 4));
    if (const TexAdapt* a = AdaptOf(t)) {
        TexAdapt copy = *a;
        HRESULT hr = AdaptedLockRect(t, face * 64 + level, copy, level, rect, Arg(c, 5), Arg(c, 3),
                                     [&](UINT host_level, D3DLOCKED_RECT* lr) {
                                         return t->LockRect(D3DCUBEMAP_FACES(face), host_level, lr, rect, Arg(c, 5));
                                     });
        Return(c, uint32_t(hr), 6);
        return;
    }
    D3DLOCKED_RECT lr;
    HRESULT hr = t->LockRect(D3DCUBEMAP_FACES(face), level, &lr, rect, Arg(c, 5));
    D3DSURFACE_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockRectCommon(c, t, face * 64 + level, hr, lr, desc, rect, Arg(c, 5), Arg(c, 3), 6);
}
M(IDirect3DCubeTexture9, UnlockRect) {
    auto* t = Host<IDirect3DCubeTexture9>(Arg(c, 0));
    uint32_t face = Arg(c, 1), level = Arg(c, 2);
    const TexAdapt* a = AdaptOf(t);
    bool host_locked = true;
    if (a && AdaptedUnlock(t, face * 64 + level, &host_locked)) {
        Return(c, host_locked ? uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(face), level - a->skip)) : 0u, 3);
        return;
    }
    ShadowUnlock(t, face * 64 + level);
    Return(c, uint32_t(t->UnlockRect(D3DCUBEMAP_FACES(face), a ? level - a->skip : level)), 3);
}

// D3DLOCKED_BOX in guest memory: {INT RowPitch; INT SlicePitch; void* pBits} (12 bytes).
void LockBoxCommon(Ctx* c, void* object, uint32_t sub, HRESULT hr, const D3DLOCKED_BOX& lb, uint32_t depth,
                   DWORD flags, uint32_t out, int args) {
    if (SUCCEEDED(hr)) {
        W32(out, uint32_t(lb.RowPitch));
        W32(out + 4, uint32_t(lb.SlicePitch));
        W32(out + 8, ShadowLock(object, sub, lb.pBits, uint32_t(lb.SlicePitch) * depth, flags));
    }
    Return(c, uint32_t(hr), args);
}
M(IDirect3DVolumeTexture9, LockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    uint32_t level = Arg(c, 1);
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 3));
    D3DLOCKED_BOX lb;
    HRESULT hr = t->LockBox(level, &lb, box, Arg(c, 4));
    D3DVOLUME_DESC desc;
    t->GetLevelDesc(level, &desc);
    LockBoxCommon(c, t, level, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 4), Arg(c, 2), 5);
}
M(IDirect3DVolumeTexture9, UnlockBox) {
    auto* t = Host<IDirect3DVolumeTexture9>(Arg(c, 0));
    ShadowUnlock(t, Arg(c, 1));
    Return(c, uint32_t(t->UnlockBox(Arg(c, 1))), 2);
}
M(IDirect3DVolume9, LockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    const D3DBOX* box = Ptr<D3DBOX>(Arg(c, 2));
    D3DLOCKED_BOX lb;
    HRESULT hr = v->LockBox(&lb, box, Arg(c, 3));
    D3DVOLUME_DESC desc;
    v->GetDesc(&desc);
    LockBoxCommon(c, v, 0, hr, lb, box ? box->Back - box->Front : desc.Depth, Arg(c, 3), Arg(c, 1), 4);
}
M(IDirect3DVolume9, UnlockBox) {
    auto* v = Host<IDirect3DVolume9>(Arg(c, 0));
    ShadowUnlock(v, 0);
    Return(c, uint32_t(v->UnlockBox()), 1);
}

template <typename T, typename Desc>
void BufferLock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    uint32_t offset = Arg(c, 1), size = Arg(c, 2), out = Arg(c, 3);
    DWORD flags = Arg(c, 4);
    void* p = nullptr;
    HRESULT hr = b->Lock(offset, size, &p, flags);
    if (SUCCEEDED(hr)) {
        Desc d;
        b->GetDesc(&d);
        uint32_t reach = d.Size > offset ? d.Size - offset : size;
        // Setting lock_stats = 1: where buffer lock copies come from (whole-buffer locks vs ranges, by flags).
        static const bool stats = SettingInt("lock_stats", 0) != 0;
        if (stats) {
            static std::mutex m;
            static uint64_t n[4], bytes[4], calls = 0;  // [whole|range][discard/nooverwrite|plain]
            std::lock_guard<std::mutex> l(m);
            int k = (size ? 2 : 0) + ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) ? 0 : 1);
            ++n[k];
            bytes[k] += size ? size : reach;
            if (++calls % 20000 == 0)
                Log("lockstats: whole dyn %llu/%llu MB, whole plain %llu/%llu MB, range dyn %llu/%llu MB, range plain %llu/%llu MB, last usage %lx size %u",
                    (unsigned long long)n[0], (unsigned long long)(bytes[0] >> 20), (unsigned long long)n[1], (unsigned long long)(bytes[1] >> 20),
                    (unsigned long long)n[2], (unsigned long long)(bytes[2] >> 20), (unsigned long long)n[3], (unsigned long long)(bytes[3] >> 20),
                    (unsigned long)d.Usage, d.Size);
        }
        if (!size) size = reach;
        static const bool mirror = SettingInt("buffer_mirror", 1) != 0;
        uint32_t guest = mirror && offset < d.Size
                             ? MirrorLock(b, d.Size, offset, std::min(size, d.Size - offset), p, flags) : 0;
        W32(out, guest ? guest : ShadowLock(b, 0, p, size, flags, reach));
    }
    Return(c, uint32_t(hr), 5);
}
template <typename T>
void BufferUnlock(Ctx* c) {
    auto* b = Host<T>(Arg(c, 0));
    if (!MirrorUnlock(b)) ShadowUnlock(b, 0);
    Return(c, uint32_t(b->Unlock()), 1);
}
// ProcessVertices writes the destination buffer on the device's side: its mirror no longer equals it.
M(IDirect3DDevice9, ProcessVertices) {
    auto* dest = Unwrap<IDirect3DVertexBuffer9>(Arg(c, 4));
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto it = g_mirrors.find(dest);
        if (it != g_mirrors.end()) it->second.valid = false;
    }
    auto r = Host<IDirect3DDevice9>(Arg(c, 0))->ProcessVertices((UINT)Arg(c, 1), (UINT)Arg(c, 2), (UINT)Arg(c, 3), dest,
                                                                Unwrap<IDirect3DVertexDeclaration9>(Arg(c, 5)), (DWORD)Arg(c, 6));
    Return(c, (uint32_t)r, 7);
}
M(IDirect3DVertexBuffer9, Lock) { BufferLock<IDirect3DVertexBuffer9, D3DVERTEXBUFFER_DESC>(c); }
M(IDirect3DVertexBuffer9, Unlock) { BufferUnlock<IDirect3DVertexBuffer9>(c); }
M(IDirect3DIndexBuffer9, Lock) { BufferLock<IDirect3DIndexBuffer9, D3DINDEXBUFFER_DESC>(c); }
M(IDirect3DIndexBuffer9, Unlock) { BufferUnlock<IDirect3DIndexBuffer9>(c); }

#undef M

// Debug (setting rt_stats = 1): draws and primitives per render target size and format, logged every 5 s; setting
// skip_rt = <w>x<h> drops the draws into targets of that size, to measure what a pass costs on the GPU.
struct RtStat { uint64_t draws = 0, prims = 0, sets = 0; };
std::map<std::string, RtStat> g_rt_stats;
std::string g_rt_key = "backbuffer";
uint32_t g_cur_ps = 0, g_cur_vs = 0;
std::set<uint32_t> g_skip_ps;
std::map<uint32_t, RtStat> g_ps_stats, g_vs_stats;  // the depth pass, by shader
// A trivial pixel shader (mov oC0, (1,0,0,0)) that the R32F depth pass uses for a few small draws. On Mali these draws
// stall the GPU for most of a frame while the picture is the same without them, so they are dropped on Android
// (setting skip_depth_ps = 0 keeps them).
const uint32_t kSlowDepthPs[] = {0xf01be99e};
std::vector<uint32_t> g_slow_handles;
std::vector<uint32_t> g_skip_handles;  // skip_ps
bool g_cur_ps_skip = false;
std::map<uint32_t, uint32_t> g_shader_hash;  // guest handle -> FNV of the bytecode: handles change between runs
// Setting dump_shaders = 1: every shader's bytecode goes to shaders/<ps|vs>_<hash>.bin and its handle to the log.
// ---- shader prewarm ----
// The game creates shaders when a material first shows up; DXVK then translates the bytecode and compiles pipeline
// libraries, and the first draws wait for them (hitches of up to a second). Every shader the game creates is kept in shader_cache.bin (the working folder),
// and the next run creates them all ahead from the render thread, a few ms a frame from the first one on (setting
// shader_prewarm, ms a frame, default 6, 0 = off): DXVK keeps modules by the bytecode's hash, so the game's own creation
// later finds the compiled one.
std::mutex g_shcat_mutex;
std::unordered_set<uint64_t> g_shcat_known;  // stage bit << 32 | FNV hash
FILE* g_shcat_file = nullptr;
bool g_shcat_loaded = false;
struct CachedShader { bool pixel; std::vector<uint32_t> code; };
std::vector<CachedShader> g_prewarm;  // read at the first frame, created over the next ones
size_t g_prewarm_next = 0;  // the objects are released at once: DXVK keeps the compiled modules by hash

void ShaderCacheLoad() {
    std::lock_guard<std::mutex> lock(g_shcat_mutex);
    if (g_shcat_loaded) return;
    g_shcat_loaded = true;
    if (FILE* f = fopen("shader_cache.bin", "rb")) {
        for (;;) {
            uint32_t head[3];  // pixel, FNV hash, dwords
            if (fread(head, 4, 3, f) != 3 || head[2] == 0 || head[2] > 65536) break;
            CachedShader s{head[0] != 0, std::vector<uint32_t>(head[2])};
            if (fread(s.code.data(), 4, head[2], f) != head[2]) break;
            if (g_shcat_known.insert(uint64_t(head[0] != 0) << 32 | head[1]).second) g_prewarm.push_back(std::move(s));
        }
        fclose(f);
    }
    g_shcat_file = fopen("shader_cache.bin", "ab");
    Log("d3d9: shader cache: %zu shaders to create ahead", g_prewarm.size());
}

void ShaderCatalogue(bool pixel, uint32_t hash, uint32_t code, uint32_t dwords) {
    if (!dwords || dwords > 65536) return;
    std::lock_guard<std::mutex> lock(g_shcat_mutex);
    if (!g_shcat_file || !g_shcat_known.insert(uint64_t(pixel) << 32 | hash).second) return;
    uint32_t head[3] = {uint32_t(pixel), hash, dwords};
    fwrite(head, 4, 3, g_shcat_file);
    fwrite(Ptr<void>(code), 4, dwords, g_shcat_file);
    fflush(g_shcat_file);
}

void ShaderPrewarmStep(IDirect3DDevice9* dev) {
    // a time budget a frame (ms): menus and loading screens, where frames are cheap, get through most of it
    static const int budget_ms = SettingInt("shader_prewarm", 6);
    if (budget_ms <= 0 || !dev) return;
    if (!g_shcat_loaded) ShaderCacheLoad();
    static auto started = std::chrono::steady_clock::now();
    const auto frame_start = std::chrono::steady_clock::now();
    while (g_prewarm_next < g_prewarm.size() &&
           std::chrono::steady_clock::now() - frame_start < std::chrono::milliseconds(budget_ms)) {
        const CachedShader& s = g_prewarm[g_prewarm_next++];
        if (s.pixel) {
            IDirect3DPixelShader9* p = nullptr;
            if (SUCCEEDED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(s.code.data()), &p)) && p) p->Release();
        } else {
            IDirect3DVertexShader9* v = nullptr;
            if (SUCCEEDED(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(s.code.data()), &v)) && v) v->Release();
        }
        if (g_prewarm_next == g_prewarm.size()) {
            Log("d3d9: shader cache: %zu shaders created ahead in %.1f s", g_prewarm.size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            std::vector<CachedShader>().swap(g_prewarm);
            g_prewarm_next = 0;
        }
    }
}

// The game's own CreateVertexShader / CreatePixelShader calls (d3d9_gen.inc): time spent inside DXVK.
void ShaderCreateTime(std::chrono::steady_clock::time_point t0) {
    g_shader_create_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                                 std::memory_order_relaxed);
    g_shader_creates.fetch_add(1, std::memory_order_relaxed);
}

void ShaderCreated(bool pixel, uint32_t handle, uint32_t code) {
    g_hitch_shaders.fetch_add(1, std::memory_order_relaxed);
    uint32_t h = 2166136261u, n = 0;
    for (; n < 65536; ++n) {
        uint32_t t = R32(code + n * 4);
        h = (h ^ t) * 16777619u;
        if (t == 0x0000ffffu) { ++n; break; }
    }
    g_shader_hash[handle] = h;
    ShaderCatalogue(pixel, h, code, n);
    for (uint32_t slow : kSlowDepthPs)
        if (pixel && slow == h) g_slow_handles.push_back(handle);
    // skip_ps = hex hashes (comma separated): pixel shaders whose draws are dropped in every pass (a shader found
    // to stall the GPU for seconds by the GPU profiler's per-draw drill-down)
    static const std::vector<uint32_t> skip = [] {
        std::vector<uint32_t> v;
        std::string list = Setting("skip_ps", "");
        for (size_t i = 0; i < list.size();) {
            size_t j = list.find_first_of(", ", i);
            if (j == std::string::npos) j = list.size();
            if (j > i) v.push_back(uint32_t(strtoul(list.substr(i, j - i).c_str(), nullptr, 16)));
            i = j + 1;
        }
        return v;
    }();
    for (uint32_t k : skip)
        if (pixel && k == h) { g_skip_handles.push_back(handle); Log("shader: pixel shader %08x (handle %08x) is dropped (skip_ps)", h, handle); }
    static const bool dump = SettingInt("dump_shaders", 0) != 0;
    if (!dump) return;
    mkdir("shaders", 0775);
    char name[64];
    snprintf(name, sizeof name, "shaders/%s_%08x.bin", pixel ? "ps" : "vs", h);
    if (FILE* f = fopen(name, "wb")) {
        fwrite(G(code), 4, n, f);
        fclose(f);
    }
    Log("shader %s handle %08x hash %08x, %u tokens", pixel ? "ps" : "vs", handle, h, n);
}
// Switch between render variants in one scene (for comparisons in a single run): the file ab_off in the
// working directory names the variant that is active now; it is read every 2 s, and each switch logs the fps of the
// interval before it ("ab: ..."). Variants: aniso (no anisotropic filtering),
// prepass (R32F depth pre-pass draws dropped), post (every target of at most 640 wide dropped), shadows (shadow map
// draws dropped), env (one environment cube face a frame), occwait (honest occlusion queries).
std::atomic<uint32_t> g_occ_pending{0}, g_occ_pending_total{0}, g_env_skipped{0}, g_shadow_skipped{0};  // occlusion results not ready when asked (honest mode)
int g_ab = kAbNone;
bool g_rt_is_small = false;   // a target of at most 640 wide that is not a shadow map's dummy (HDR, bloom, cube faces)
bool g_rt_is_shadow = false;  // the 32x32 dummy colour target with a D16 shadow map of 512+ bound: a shadow map pass
uint64_t g_ab_frame = 0;
bool g_cur_ps_slow = false, g_rt_is_depth = false, g_rt_is_occlusion = false, g_rt_is_32 = false, g_ds_is_big = false;
D3DSURFACE_DESC g_rt_desc;  // the bound colour target 0 (Width 0: none)
D3DSURFACE_DESC g_ds_desc;  // the bound depth buffer (Width 0: none)
// Environment cube maps. Every frame the engine renders its two reflection cube maps (128x128 and 32x32, 6 faces each,
// 3 draws a face with a Clear): 12 render passes a frame, and on tiled mobile GPUs every pass has a fixed cost whatever
// its size. Setting env_faces_per_frame (Android default 1, desktop 6 = all): only that
// many faces of each cube are drawn in a frame, round-robin, so a cube is refreshed every 6/N frames; the other faces
// keep their previous content (their draws and clears are dropped). Sky and distant scenery in a reflection change
// slowly, so a refresh every 0.1-0.2 s is not visible.
bool g_rt_is_env = false;    // colour target 0 is a face of a cube texture
bool g_rt_env_skip = false;  // ... and that face is not drawn this frame
std::unordered_map<void*, uint32_t> g_env_binds;  // cube texture -> faces bound this frame (render thread only)
int EnvFacesPerFrame() {
#ifdef __ANDROID__
    static const int n = SettingInt("env_faces_per_frame", 1);
#else
    static const int n = SettingInt("env_faces_per_frame", 6);
#endif
    return g_ab == kAbEnv ? 1 : (n < 1 ? 1 : n);
}
bool IsCubeFace(IDirect3DSurface9* s, IDirect3DCubeTexture9** cube_out = nullptr) {
    if (!s) return false;
    IDirect3DCubeTexture9* cube = nullptr;  // DXVK: a cube face's container is its cube texture, other surfaces answer no
    if (FAILED(s->GetContainer(__uuidof(IDirect3DCubeTexture9), reinterpret_cast<void**>(&cube))) || !cube) return false;
    cube->Release();  // the texture stays alive: it is the bound render target
    if (cube_out) *cube_out = cube;
    return true;
}
// The derived pass flags, from colour target 0 and the depth buffer (set in either order by the engine).
void RtClassify() {
    g_rt_is_32 = g_rt_desc.Width == 32 && g_rt_desc.Height == 32;
    g_rt_is_occlusion = g_rt_is_32 && !g_ds_is_big;
    g_rt_is_shadow = g_rt_is_32 && g_ds_desc.Format == D3DFMT_D16 && g_ds_desc.Width >= 512;
    g_rt_is_small = g_rt_desc.Width != 0 && g_rt_desc.Width <= 640 && !g_rt_is_shadow;
}
// The 32x32 target takes the occlusion test draws (thousands of draws a frame).
// Dropped on Android; the queries then always report "visible" (GetData), so nothing is culled by mistake.
// The same 32x32 target is also the dummy colour buffer of the shadow map passes (depth 512x512 / 1024x1024 D16):
// those must be drawn, or the shadow maps stay cleared and light shines through walls.
bool SkipOcclusionPass() {
#ifdef __ANDROID__
    static const bool on = SettingInt("skip_occlusion_pass", 1) != 0;
#else
    static const bool on = SettingInt("skip_occlusion_pass", 0) != 0;
#endif
    return on;
}
void PsChanged(uint32_t handle) {
    g_cur_ps = handle;
#ifdef __ANDROID__
    static const bool on = SettingInt("skip_depth_ps", 1) != 0;
#else
    static const bool on = SettingInt("skip_depth_ps", 0) != 0;
#endif
    g_cur_ps_slow = false;
    g_cur_ps_skip = false;
    for (uint32_t h : g_skip_handles) g_cur_ps_skip |= h == handle;  // usually empty
    if (!on) return;
    for (uint32_t h : g_slow_handles) g_cur_ps_slow |= h == handle;  // normally one entry; this runs on every shader change
}
std::string g_rt_skip;  // re-read from rt_skip.txt every 5 s: passes can be switched off in a running level
bool GpuProfileOn();
bool g_gpu_capturing = false;
uint32_t g_gpu_draws = 0;
bool RtStatsOn() { static const bool on = SettingInt("rt_stats", 0) != 0; return on; }
void RtTrack(uint32_t index, IDirect3DSurface9* s) {
    if (index != 0) return;
    D3DSURFACE_DESC d{};
    g_rt_desc = s && SUCCEEDED(s->GetDesc(&d)) ? d : D3DSURFACE_DESC{};
    g_rt_is_depth = g_rt_desc.Format == D3DFMT_R32F;
    IDirect3DCubeTexture9* cube = nullptr;
    g_rt_is_env = IsCubeFace(s, &cube);
    g_rt_env_skip = false;
    if (g_rt_is_env) {
        const int per = EnvFacesPerFrame();
        if (per < 6) {
            const uint32_t idx = g_env_binds[cube]++;  // the k-th face of this cube bound this frame
            g_rt_env_skip = int((idx + g_frames.load(std::memory_order_relaxed)) % 6) >= per;
        }
    }
    RtClassify();
    if (!RtStatsOn() && !GpuProfileOn()) return;
    char b[48] = "null";
    if (s && SUCCEEDED(s->GetDesc(&d))) snprintf(b, sizeof b, "%ux%u", d.Width, d.Height);
    g_rt_key = b;
    if (s) { snprintf(b, sizeof b, "/%u", unsigned(d.Format)); ++g_rt_stats[g_rt_key + b].sets; }
    if (s) g_rt_key += b;
}
// The shadow map passes bind their depth map (512x512, 1024x1024) with a 32x32 colour target that nothing reads. DXVK
// renders only the area every attachment covers, 32x32: the rest of the shadow map stayed cleared and lights and
// shadows came out as streaks. Such passes get a colour target of the NULL format (no memory, no attachment in
// DXVK) as large as the depth map instead.
IDirect3DSurface9* ShadowColorTarget(IDirect3DDevice9* dev, uint32_t index, IDirect3DSurface9* s) {
    static const bool on = SettingInt("shadow_null_rt", 1) != 0;
    if (!on || index != 0 || !s || !g_ds_desc.Width) return s;
    D3DSURFACE_DESC d{};
    if (FAILED(s->GetDesc(&d)) || d.Width != 32 || d.Height != 32) return s;
    if (g_ds_desc.Width <= 32 || g_ds_desc.Format == D3DFMT_D24S8) return s;  // cube faces, HDR: real depth+stencil
    static std::map<std::pair<UINT, UINT>, IDirect3DSurface9*> nulls;
    IDirect3DSurface9*& n = nulls[{g_ds_desc.Width, g_ds_desc.Height}];
    if (!n) {
        HRESULT r = dev->CreateRenderTarget(g_ds_desc.Width, g_ds_desc.Height, D3DFORMAT(MAKEFOURCC('N', 'U', 'L', 'L')),
                                            D3DMULTISAMPLE_NONE, 0, FALSE, &n, nullptr);
        Log("d3d9: NULL colour target %ux%u for shadow passes: %s", g_ds_desc.Width, g_ds_desc.Height, SUCCEEDED(r) ? "ok" : "failed");
        if (FAILED(r)) n = s;
    }
    return n;
}
// Lazy depth buffer (setting lazy_ds, off by default). The game keeps its depth buffer bound through many
// passes that never test or write depth (post-processing, HDR, bloom), and on a tiled GPU every pass with a depth
// attachment loads and stores it: DXVK reported 52 render passes a frame with the GPU busy all the time. The depth
// buffer the game sets is only handed to DXVK once a draw has depth or stencil enabled (or a Clear touches it), and
// stays bound until the game changes it. Before the game's first SetDepthStencilSurface (and after a Reset) calls
// pass straight through.
bool g_lazy_off_now = false;  // switch while running: a file lazy_ds_off in the working directory (checked every 2 s)
bool LazyDsOn() {
    // Off by default: it made no difference (the passes' cost is not the depth loads).
    static const bool on = SettingInt("lazy_ds", 0) != 0;
    return on && !g_lazy_off_now;
}
bool g_lazy_active = false;              // the game set a depth buffer since the device was created or reset
IDirect3DSurface9* g_lazy_game = nullptr;   // what the game has bound
IDirect3DSurface9* g_lazy_bound = nullptr;  // what DXVK has bound
bool g_z_enable = true, g_stencil_enable = false;
// Holds a reference on the game's depth buffer while it is not bound in DXVK (so it cannot go away unseen); dropped
// before a Reset, which fails while default-pool resources are referenced.
void LazyDsReset() {
    if (g_lazy_game) g_lazy_game->Release();
    g_lazy_active = false;
    g_lazy_game = g_lazy_bound = nullptr;
    g_z_enable = true;
    g_stencil_enable = false;
}
// SetDepthStencilSurface: true = handled here (nothing to pass on).
bool LazyDsSet(IDirect3DDevice9* dev, IDirect3DSurface9* s) {
    if (!LazyDsOn()) return false;
    if (!g_lazy_active) {
        g_lazy_active = true;
        IDirect3DSurface9* cur = nullptr;  // what DXVK has now (the automatic depth buffer)
        if (SUCCEEDED(dev->GetDepthStencilSurface(&cur)) && cur) { g_lazy_bound = cur; cur->Release(); }
    }
    if (s) s->AddRef();
    if (g_lazy_game) g_lazy_game->Release();
    g_lazy_game = s;
    if (g_lazy_bound && g_lazy_bound != s) {  // never leave another (maybe smaller) depth buffer bound
        dev->SetDepthStencilSurface(nullptr);
        g_lazy_bound = nullptr;
    }
    return true;
}
void LazyDsPoll(IDirect3DDevice9* dev) {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    bool off = access("lazy_ds_off", F_OK) == 0;
    if (off == g_lazy_off_now) return;
    static auto since = now;
    static uint64_t since_frame = 0;
    uint64_t frame = g_frames.load(std::memory_order_relaxed);
    Log("lazy_ds: %s -> %s; the last %.0f s ran at %.1f fps", g_lazy_off_now ? "off" : "on", off ? "off" : "on",
        std::chrono::duration<double>(now - since).count(),
        double(frame - since_frame) / std::chrono::duration<double>(now - since).count());
    since = now;
    since_frame = frame;
    if (off && g_lazy_active) {  // hand the game's depth buffer back and pass calls through again
        dev->SetDepthStencilSurface(g_lazy_game);
        if (g_lazy_game) g_lazy_game->Release();
        g_lazy_active = false;
        g_lazy_game = g_lazy_bound = nullptr;
    }
    g_lazy_off_now = off;
}
void AbPoll(uint64_t frame) {
    g_ab_frame = frame;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    static const char* const kNames[] = {"none", "aniso", "prepass", "post", "shadows", "env", "occwait"};
    int mode = kAbNone;
    if (FILE* f = fopen("ab_off", "r")) {
        char b[32] = "";
        if (fgets(b, sizeof b, f)) {
            b[strcspn(b, "\r\n ")] = 0;
            for (int i = 1; i < int(sizeof kNames / sizeof *kNames); ++i) if (strcmp(b, kNames[i]) == 0) mode = i;
        }
        fclose(f);
    }
    if (mode == g_ab) return;
    static auto since = now;
    static uint64_t since_frame = frame;
    double secs = std::chrono::duration<double>(now - since).count();
    Log("ab: %s for %.0f s: %.1f fps (occlusion not ready %u times)", kNames[g_ab], secs, secs > 0 ? double(frame - since_frame) / secs : 0.0,
        g_occ_pending.exchange(0));
    since = now;
    since_frame = frame;
    g_ab = mode;
}
void LazyDsNeed(IDirect3DDevice9* dev) {
    if (!g_lazy_active || g_lazy_bound == g_lazy_game) return;
    dev->SetDepthStencilSurface(g_lazy_game);
    g_lazy_bound = g_lazy_game;
}
void LazyDsDraw(IDirect3DDevice9* dev) {
    if (g_lazy_active && g_lazy_bound != g_lazy_game && (g_z_enable || g_stencil_enable)) LazyDsNeed(dev);
}
void LazyDsState(uint32_t state, uint32_t value) {
    if (state == D3DRS_ZENABLE) g_z_enable = value != 0;
    else if (state == D3DRS_STENCILENABLE) g_stencil_enable = value != 0;
}
HRESULT LazyDsGet(IDirect3DDevice9* dev, IDirect3DSurface9** out) {
    if (!g_lazy_active || !out) return dev->GetDepthStencilSurface(out);
    *out = g_lazy_game;
    if (!g_lazy_game) return D3DERR_NOTFOUND;
    g_lazy_game->AddRef();
    return D3D_OK;
}
// Shadow maps refreshed every other frame. Outdoors the sun's shadow maps are a large part of the GPU frame (8 passes
// of a few hundred draws). Setting shadow_far_every (default 1 =
// off): shadow passes after the first shadow_near_maps (default 1, the nearest cascade comes first) are drawn only
// every N-th frame, staggered; skipped ones keep last frame's map (draws and clears dropped). A depth map the game binds
// more than once in a frame (shared between lights) is never skipped: its content would belong to another light.
uint32_t g_shadow_seq = 0;                             // shadow passes bound this frame (render thread only)
bool g_ds_shadow_skip = false;                         // the bound shadow map is not redrawn this frame
std::unordered_map<void*, uint32_t> g_shadow_binds;    // depth map -> binds this frame
std::unordered_set<void*> g_shadow_shared;             // depth maps bound twice in one frame: never skipped
std::string g_shadow_order;                            // the frame's shadow passes, logged every 600 frames
// Off (1) unless shadow_cache = 0: with the game's own shadow map cache on (m2_imports.cpp Hook_RendererSetup) a far
// map is drawn only when it changed, and dropping that draw would leave it stale until the next change.
int ShadowFarEvery() {
    static const int n = SettingInt("shadow_cache", 1) != 0 ? 1 : SettingInt("shadow_far_every", 1);
    return n < 1 ? 1 : n;
}
void DsTrack(IDirect3DSurface9* s) {
    D3DSURFACE_DESC d{};
    g_ds_is_big = s && SUCCEEDED(s->GetDesc(&d)) && d.Width > 32;
    g_ds_desc = s ? d : D3DSURFACE_DESC{};
    g_ds_shadow_skip = false;
    if (s && d.Format == D3DFMT_D16 && d.Width >= 512) {
        const uint32_t seq = g_shadow_seq++;
        if (++g_shadow_binds[s] > 1) g_shadow_shared.insert(s);
        static const int near_maps = SettingInt("shadow_near_maps", 1);
        const int every = ShadowFarEvery();
        const uint64_t frame = g_frames.load(std::memory_order_relaxed);
        g_ds_shadow_skip = every > 1 && int(seq) >= near_maps && !g_shadow_shared.count(s) &&
                           (frame + seq) % uint64_t(every) != 0;
        if (frame % 600 == 0) {
            char b[64];
            snprintf(b, sizeof b, " %u:%p:%u%s", seq, (void*)s, d.Width, g_ds_shadow_skip ? "(skip)" : "");
            g_shadow_order += b;
        }
    }
    RtClassify();
}
void EnvFrameReset() {
    g_env_binds.clear();
    if (!g_shadow_order.empty()) {
        Log("d3d9: shadow passes this frame:%s; shared maps %zu", g_shadow_order.c_str(), g_shadow_shared.size());
        g_shadow_order.clear();
    }
    g_shadow_seq = 0;
    g_shadow_binds.clear();
}
void GpuDrillDraw(uint32_t prims);
bool RtSkip(uint32_t prims) {
    ++g_frame_draws;
    if (g_rt_is_shadow) ++g_frame_shadow_draws;
    // The same draws also come in a 640x360 colour pass (after loads, in cutscenes), where they stall the GPU too.
    if (g_cur_ps_slow) return true;
    if (g_cur_ps_skip) return true;
    if (g_rt_env_skip) { g_env_skipped.fetch_add(1, std::memory_order_relaxed); return true; }  // a cube face that is not this frame's (env_faces_per_frame)
    if (g_rt_is_shadow && g_ds_shadow_skip) { g_shadow_skipped.fetch_add(1, std::memory_order_relaxed); return true; }  // shadow_far_every
    if (g_gpu_capturing) { GpuDrillDraw(prims); ++g_gpu_draws; }
    if (g_rt_is_occlusion && SkipOcclusionPass()) return true;
    switch (g_ab) {
        case kAbPrepass: if (g_rt_is_depth) return true; break;
        case kAbPost: if (g_rt_is_small) return true; break;
        case kAbShadows: if (g_rt_is_shadow) return true; break;
        default: break;
    }
    if (!RtStatsOn()) return false;
    RtStat& st = g_rt_stats[g_rt_key];
    ++st.draws;
    st.prims += prims;
    ++g_ps_stats[g_cur_ps].draws; g_ps_stats[g_cur_ps].prims += prims;
    ++g_vs_stats[g_cur_vs].draws; g_vs_stats[g_cur_vs].prims += prims;
    if (g_skip_ps.count(g_cur_ps)) return true;
    return g_rt_skip == "all" || (!g_rt_skip.empty() && g_rt_key.rfind(g_rt_skip, 0) == 0);
}
// Clear: dropped together with the draws of a skipped cube face (a cleared face would go black until its turn).
bool ClearSkip() { return g_rt_env_skip || (g_rt_is_shadow && g_ds_shadow_skip); }
void MemmoveStatsLog();  // m2_imports.cpp
void RtLog() {
    if (!RtStatsOn()) return;
    static auto last = std::chrono::steady_clock::now();
    static uint64_t frames = 0;
    ++frames;
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    g_rt_skip.clear();
    g_skip_ps.clear();
    if (FILE* f = fopen("rt_skip.txt", "r")) {
        char b[64] = "";
        if (fgets(b, sizeof b, f)) { b[strcspn(b, "\r\n ")] = 0; g_rt_skip = b; }
        // further lines: pixel shader handles (hex) whose draws are dropped in every pass, for bisecting a slow scene
        g_skip_ps.clear();
        unsigned h;
        while (fscanf(f, "%x", &h) == 1) g_skip_ps.insert(h);
        fclose(f);
    }
    Log("rt: skip '%s' +%zu shaders, %.1f fps", g_rt_skip.c_str(), g_skip_ps.size(), double(frames) / 5.0);
    MemmoveStatsLog();
    for (auto& [k, v] : g_rt_stats)
        if (v.draws || v.sets) Log("rt %s: %llu draws %llu prims %llu sets / %llu frames", k.c_str(), (unsigned long long)v.draws, (unsigned long long)v.prims, (unsigned long long)v.sets, (unsigned long long)frames);
    for (auto* m : {&g_ps_stats, &g_vs_stats}) {
        for (auto& [k, v] : *m)
            if (v.draws * 50 >= 1) Log("sh %s%08x hash %08x: %llu draws %llu prims", m == &g_ps_stats ? "ps" : "vs", k, g_shader_hash.count(k) ? g_shader_hash[k] : 0, (unsigned long long)v.draws, (unsigned long long)v.prims);
        m->clear();
    }
    g_rt_stats.clear();
    frames = 0;
}

// GPU profiler (setting gpu_profile = 1): every 5 s one frame gets a GPU timestamp at each render target change and at
// its end; a few frames later the results give the GPU time of each pass, logged as "gpu:" lines with the pass's
// target, format and draw count. Per pass, not per draw: on a tiler a timestamp inside a pass means little.
struct GpuStamp { IDirect3DQuery9* q; std::string label; uint32_t draws; };
std::vector<GpuStamp> g_gpu_stamps;
IDirect3DQuery9* g_gpu_freq = nullptr;
uint64_t g_gpu_frame = 0, g_gpu_collect_at = 0;
std::string g_gpu_label = "frame start";
bool GpuProfileOn() { static const bool on = SettingInt("gpu_profile", 0) != 0; return on; }
// Drill-down: once a pass took over 200 ms, later captures put a timestamp before every draw of passes with that
// target (up to 4096 a frame) and log the slowest draws with their shaders: the way to find a shader that stalls the
// GPU for seconds (then dropped with skip_ps = <hash>).
std::string g_gpu_drill;
IDirect3DDevice9* g_gpu_dev = nullptr;
uint32_t g_drill_stamps = 0;
void GpuStampNow(IDirect3DDevice9* dev, const std::string& next_label);
void GpuDrillDraw(uint32_t prims) {
    if (g_gpu_drill.empty() || g_rt_key != g_gpu_drill || !g_gpu_dev || g_drill_stamps >= 4096) return;
    ++g_drill_stamps;
    char b[96];
    snprintf(b, sizeof b, "draw ps %08x vs %08x, %u prims", g_shader_hash.count(g_cur_ps) ? g_shader_hash[g_cur_ps] : 0,
             g_shader_hash.count(g_cur_vs) ? g_shader_hash[g_cur_vs] : 0, prims);
    GpuStampNow(g_gpu_dev, b);
}
void GpuStampNow(IDirect3DDevice9* dev, const std::string& next_label) {
    IDirect3DQuery9* q = nullptr;
    if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q)) && q) {
        q->Issue(D3DISSUE_END);
        g_gpu_stamps.push_back({q, g_gpu_label, g_gpu_draws});
    }
    g_gpu_label = next_label;
    g_gpu_draws = 0;
}
void GpuMark(IDirect3DDevice9* dev, uint32_t index) {
    if (!g_gpu_capturing || index != 0) return;
    GpuStampNow(dev, g_rt_key);
}
// Called from Present: starts a capture now and then, closes the frame being captured, collects finished ones.
void GpuFrame(IDirect3DDevice9* dev, uint64_t frame) {
    if (!GpuProfileOn()) return;
    static auto last = std::chrono::steady_clock::now();
    if (g_gpu_capturing) {
        GpuStampNow(dev, "after present");
        g_gpu_capturing = false;
        g_gpu_collect_at = frame + 3;
    } else if (g_gpu_collect_at && frame >= g_gpu_collect_at) {
        std::vector<uint64_t> t(g_gpu_stamps.size(), 0);
        uint64_t freq = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        bool ok = true;
        while (g_gpu_freq->GetData(&freq, sizeof freq, D3DGETDATA_FLUSH) == S_FALSE && std::chrono::steady_clock::now() < deadline) {}
        for (size_t k = 0; k < g_gpu_stamps.size(); ++k)
            while (true) {
                HRESULT r = g_gpu_stamps[k].q->GetData(&t[k], sizeof t[k], D3DGETDATA_FLUSH);
                if (r == S_OK) break;
                if (r != S_FALSE || std::chrono::steady_clock::now() > deadline) { ok = false; break; }
            }
        if (ok && freq && g_gpu_stamps.size() > 1) {
            std::vector<std::pair<double, size_t>> pass;
            for (size_t k = 1; k < g_gpu_stamps.size(); ++k)
                pass.push_back({double(t[k] - t[k - 1]) * 1000.0 / double(freq), k});
            double total = double(t.back() - t.front()) * 1000.0 / double(freq);
            std::sort(pass.rbegin(), pass.rend());
            Log("gpu: frame %llu, %.1f ms on the GPU, %zu passes", (unsigned long long)g_gpu_frame, total, pass.size());
            size_t shown = 0;
            for (size_t k = 0; k < pass.size() && shown < 12; ++k) {
                const GpuStamp& st = g_gpu_stamps[pass[k].second];
                if (!g_gpu_drill.empty() && st.label.rfind("draw ", 0) == 0) continue;  // draws are listed below
                Log("gpu:   %6.2f ms  %s, %u draws", pass[k].first, st.label.c_str(), st.draws);
                ++shown;
            }
            if (!g_gpu_drill.empty()) {
                shown = 0;
                for (size_t k = 0; k < pass.size() && shown < 15; ++k) {
                    const GpuStamp& st = g_gpu_stamps[pass[k].second];
                    if (st.label.rfind("draw ", 0) != 0) continue;
                    Log("gpu: drill %s: %8.2f ms  %s", g_gpu_drill.c_str(), pass[k].first, st.label.c_str());
                    ++shown;
                }
                // The same per shader pair: where a pass's time goes when no single draw stands out.
                struct Sum { double ms = 0; uint32_t draws = 0; uint64_t prims = 0; };
                std::map<std::string, Sum> by;
                for (const auto& p : pass) {
                    const std::string& l = g_gpu_stamps[p.second].label;
                    if (l.rfind("draw ", 0) != 0) continue;
                    size_t comma = l.find(',');
                    Sum& s = by[l.substr(5, comma == std::string::npos ? std::string::npos : comma - 5)];
                    s.ms += p.first;
                    ++s.draws;
                    if (comma != std::string::npos) s.prims += strtoull(l.c_str() + comma + 2, nullptr, 10);
                }
                std::vector<std::pair<double, std::string>> order;
                for (const auto& kv : by) order.push_back({kv.second.ms, kv.first});
                std::sort(order.rbegin(), order.rend());
                for (size_t k = 0; k < order.size() && k < 12; ++k) {
                    const Sum& s = by[order[k].second];
                    Log("gpu: by shader %s: %7.2f ms  %s, %u draws, %llu prims", g_gpu_drill.c_str(), s.ms,
                        order[k].second.c_str(), s.draws, (unsigned long long)s.prims);
                }
            }
            // start or stop drilling (setting gpu_drill = <pass, as "1280x720/114">: always drill that pass)
            static const std::string forced = Setting("gpu_drill", "");
            if (!forced.empty()) {
                g_gpu_drill = forced;
            } else if (g_gpu_drill.empty() && !pass.empty() && pass[0].first > 200.0) {
                g_gpu_drill = g_gpu_stamps[pass[0].second].label;
                Log("gpu: %s took %.0f ms: timing each of its draws from the next capture", g_gpu_drill.c_str(), pass[0].first);
            } else if (!g_gpu_drill.empty() && total < 100.0) {
                Log("gpu: frames are fast again, drill-down of %s stopped", g_gpu_drill.c_str());
                g_gpu_drill.clear();
            }
        } else {
            Log("gpu: frame %llu: timestamps not available", (unsigned long long)g_gpu_frame);
        }
        for (auto& st : g_gpu_stamps) st.q->Release();
        g_gpu_stamps.clear();
        g_gpu_collect_at = 0;
    } else if (!g_gpu_collect_at && std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
        last = std::chrono::steady_clock::now();
        if (!g_gpu_freq && FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_gpu_freq))) return;
        g_gpu_freq->Issue(D3DISSUE_END);
        g_gpu_capturing = true;
        g_gpu_dev = dev;
        g_drill_stamps = 0;
        g_gpu_frame = frame;
        g_gpu_label = "frame start (" + g_rt_key + ")";
        g_gpu_draws = 0;
        GpuStampNow(dev, g_rt_key);
    }
}

#include "d3d9_gen.inc"

namespace {

// Debug (setting api_stats = 1): calls per IDirect3DDevice9 method, logged every 5 s with the per-frame rate, to
// find what the GPU's many near-empty render passes are (clears, StretchRect, GetRenderTargetData...).
constexpr size_t kApiSlots = 160;
ImportFn g_api_fn[kApiSlots];
const char* g_api_name[kApiSlots];
uint32_t g_api_calls[kApiSlots];  // plain: the game renders from one thread; a lost count only blurs a debug stat
bool g_api_trace;  // this frame's render target / depth / clear sequence is being written down (api_stats = 2)
void ApiTrace(size_t k, Ctx* c);
// api_stats = 3: how many draws repeat the previous draw's mesh/shaders/textures (only constants differ: what
// instancing could merge) and how many distinct meshes a frame has.
bool g_inst_on, g_inst_armed;
uint32_t g_inst_state[12], g_inst_last[12];
uint32_t g_inst_draws, g_inst_same, g_inst_sameprev_mesh;
std::unordered_map<uint64_t, uint32_t> g_inst_mesh;
// redundant calls (same value as already set): rs, samp, tex, stream, vs, ps, ib, decl, vs const registers, ps const
const char* const kRedName[] = {"rs", "samp", "tex", "stream", "vs", "ps", "ib", "decl", "vsc", "psc"};
uint32_t g_red[10], g_red_all[10];
std::unordered_map<uint64_t, uint64_t> g_red_last;
float g_red_vsc[256][4], g_red_psc[256][4];
void RedNote(int kind, uint64_t key, uint64_t val) {
    ++g_red_all[kind];
    auto [it, fresh] = g_red_last.try_emplace(uint64_t(kind) << 56 | key, val);
    if (!fresh && it->second == val) ++g_red[kind];
    it->second = val;
}
void RedConst(int kind, uint32_t start, uint32_t ptr, uint32_t count) {
    float (*shadow)[4] = kind == 8 ? g_red_vsc : g_red_psc;
    for (uint32_t i = 0; i < count && start + i < 256; ++i) {
        float v[4];
        memcpy(v, Ptr<float>(ptr + 16 * i), 16);
        ++g_red_all[kind];
        if (!memcmp(v, shadow[start + i], 16)) ++g_red[kind];
        memcpy(shadow[start + i], v, 16);
    }
}
bool g_red_on;
void RedHook(size_t k, Ctx* c) {
    const char* n = g_api_name[k];
    if (!strcmp(n, "SetRenderState")) RedNote(0, Arg(c, 1), Arg(c, 2));
    else if (!strcmp(n, "SetSamplerState")) RedNote(1, uint64_t(Arg(c, 1)) << 16 | Arg(c, 2), Arg(c, 3));
    else if (!strcmp(n, "SetTexture")) RedNote(2, Arg(c, 1), Arg(c, 2));
    else if (!strcmp(n, "SetStreamSource")) RedNote(3, Arg(c, 1), uint64_t(Arg(c, 2)) << 32 ^ uint64_t(Arg(c, 3)) << 8 ^ Arg(c, 4));
    else if (!strcmp(n, "SetVertexShader")) RedNote(4, 0, Arg(c, 1));
    else if (!strcmp(n, "SetPixelShader")) RedNote(5, 0, Arg(c, 1));
    else if (!strcmp(n, "SetIndices")) RedNote(6, 0, Arg(c, 1));
    else if (!strcmp(n, "SetVertexDeclaration")) RedNote(7, 0, Arg(c, 1));
    else if (!strcmp(n, "SetVertexShaderConstantF")) RedConst(8, Arg(c, 1), Arg(c, 2), Arg(c, 3));
    else if (!strcmp(n, "SetPixelShaderConstantF")) RedConst(9, Arg(c, 1), Arg(c, 2), Arg(c, 3));
}
void InstNote(size_t k, Ctx* c) {
    const char* n = g_api_name[k];
    if (!strcmp(n, "Present")) {
        if (!g_inst_armed) {  // recording started mid-frame: count from this Present to the next
            g_inst_armed = true; g_inst_draws = g_inst_same = g_inst_sameprev_mesh = 0; g_inst_mesh.clear();
            memset(g_inst_last, 0xff, sizeof g_inst_last);
            return;
        }
        g_inst_armed = false;
        g_inst_on = false;
        uint32_t rep = 0, top = 0;
        for (auto& [m, cnt] : g_inst_mesh) { rep += cnt - 1; top = std::max(top, cnt); }
        Log("api instancing: %u draws, %u same as previous, %u same mesh as previous (other state), %zu meshes, "
            "%u draws of an already drawn mesh, most drawn mesh %u times", g_inst_draws, g_inst_same,
            g_inst_sameprev_mesh, g_inst_mesh.size(), rep, top);
        return;
    }
    if (!strcmp(n, "SetVertexShader")) g_inst_state[0] = Arg(c, 1);
    else if (!strcmp(n, "SetPixelShader")) g_inst_state[1] = Arg(c, 1);
    else if (!strcmp(n, "SetIndices")) g_inst_state[2] = Arg(c, 1);
    else if (!strcmp(n, "SetStreamSource") && Arg(c, 1) == 0) { g_inst_state[3] = Arg(c, 2); g_inst_state[4] = Arg(c, 3); }
    else if (!strcmp(n, "SetTexture") && Arg(c, 1) < 4) g_inst_state[5 + Arg(c, 1)] = Arg(c, 2);
    else if (!strcmp(n, "DrawIndexedPrimitive")) {
        g_inst_state[9] = Arg(c, 2); g_inst_state[10] = Arg(c, 5); g_inst_state[11] = Arg(c, 6);  // base vertex, start, prims
        ++g_inst_draws;
        if (!memcmp(g_inst_state, g_inst_last, sizeof g_inst_state)) ++g_inst_same;
        else if (g_inst_state[2] == g_inst_last[2] && g_inst_state[3] == g_inst_last[3] && g_inst_state[10] == g_inst_last[10]) ++g_inst_sameprev_mesh;
        memcpy(g_inst_last, g_inst_state, sizeof g_inst_state);
        ++g_inst_mesh[uint64_t(g_inst_state[3]) << 32 ^ g_inst_state[2] ^ uint64_t(g_inst_state[10]) << 12 ^ g_inst_state[9]];
    }
}
bool g_api_timed;  // api_stats = 4: CPU time spent inside each method
uint64_t g_api_ns[kApiSlots];
template <size_t N> void ApiCounted(Ctx* c) {
    ++g_api_calls[N];
    if (g_api_trace) ApiTrace(N, c);
    if (g_inst_on) InstNote(N, c);
    if (g_red_on) RedHook(N, c);
    if (!g_api_timed) { g_api_fn[N](c); return; }
    // wall clock (vDSO, ~20 ns) instead of thread CPU time (a syscall that added ~0.5 us a call)
    const auto a = std::chrono::steady_clock::now();
    g_api_fn[N](c);
    g_api_ns[N] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a).count());
}
template <size_t... N> constexpr std::array<ImportFn, sizeof...(N)> ApiTable(std::index_sequence<N...>) {
    return {&ApiCounted<N>...};
}
constexpr std::array<ImportFn, kApiSlots> g_api_counted = ApiTable(std::make_index_sequence<kApiSlots>());

std::string g_trace;
unsigned g_trace_draws;
std::string SurfaceName(uint32_t guest) {
    if (!guest) return "null";
    char b[64];
    D3DSURFACE_DESC d{};
    if (auto* s = Unwrap<IDirect3DSurface9>(guest)) s->GetDesc(&d);
    snprintf(b, sizeof b, "%x:%ux%u/%u", guest & 0xffffff, d.Width, d.Height, unsigned(d.Format));
    return b;
}
void ApiTrace(size_t k, Ctx* c) {
    std::string n = g_api_name[k];
    if (n.rfind("Draw", 0) == 0) { ++g_trace_draws; return; }
    char b[160];
    if (n == "SetRenderTarget") snprintf(b, sizeof b, "RT%u=%s%s", Arg(c, 1), SurfaceName(Arg(c, 2)).c_str(), IsCubeFace(Unwrap<IDirect3DSurface9>(Arg(c, 2))) ? "(cube)" : "");
    else if (n == "SetDepthStencilSurface") snprintf(b, sizeof b, "DS=%s", SurfaceName(Arg(c, 1)).c_str());
    else if (n == "Clear") snprintf(b, sizeof b, "Clear(%u rects, flags %x)", Arg(c, 1), Arg(c, 3));
    else if (n == "StretchRect") snprintf(b, sizeof b, "StretchRect(%s -> %s)", SurfaceName(Arg(c, 1)).c_str(), SurfaceName(Arg(c, 3)).c_str());
    else if (n == "SetRenderState") {
        // depth test/write and colour writes: what the depth pre-pass and the passes after it rely on
        static uint32_t last[256];
        uint32_t st = Arg(c, 1), v = Arg(c, 2);
        if (st >= 256 || (st != D3DRS_ZENABLE && st != D3DRS_ZWRITEENABLE && st != D3DRS_ZFUNC && st != D3DRS_COLORWRITEENABLE) || last[st] == v) return;
        last[st] = v;
        snprintf(b, sizeof b, "%s=%u", st == D3DRS_ZENABLE ? "zen" : st == D3DRS_ZWRITEENABLE ? "zw" : st == D3DRS_ZFUNC ? "zf" : "cw", v);
    }
    else if (n == "SetTexture") {
        // a render target read back as a texture (sampler, surface of level 0) only: shows which pass consumes which
        uint32_t t = Arg(c, 2);
        if (!t) return;
        auto* bt = Unwrap<IDirect3DBaseTexture9>(t);
        IDirect3DTexture9* tex = nullptr;
        if (!bt || FAILED(bt->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) || !tex) return;
        D3DSURFACE_DESC d{};
        tex->GetLevelDesc(0, &d);
        tex->Release();
        if (!(d.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) return;
        snprintf(b, sizeof b, "tex%u=%x:%ux%u/%u", Arg(c, 1), t & 0xffffff, d.Width, d.Height, unsigned(d.Format));
    }
    else if (n == "Present" || n == "SetViewport" || n == "GetRenderTargetData" || n == "UpdateSurface" || n == "ColorFill" || n == "UpdateTexture") snprintf(b, sizeof b, "%s", n.c_str());
    else return;
    if (g_trace_draws) { g_trace += " [" + std::to_string(g_trace_draws) + " draws]"; g_trace_draws = 0; }
    g_trace += " ";
    g_trace += b;
}

}  // namespace

void ApiLog(uint64_t frame) {
    static const int mode = SettingInt("api_stats", 0);
    static const bool on = mode != 0;
    if (g_api_trace) {  // one frame recorded: write it out in pieces (logcat cuts long lines)
        g_api_trace = false;
        Log("api trace frame %llu:", (unsigned long long)frame);
        for (size_t i = 0; i < g_trace.size(); i += 900) Log("api trace: %s", g_trace.substr(i, 900).c_str());
        g_trace.clear();
    }
    static uint64_t last_frame = 0;
    static auto last = std::chrono::steady_clock::now();
    if (!on) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5) || frame == last_frame) return;
    double frames = double(frame - last_frame);
    last = now; last_frame = frame;
    std::vector<std::pair<uint32_t, size_t>> v;
    for (size_t k = 0; k < kApiSlots; ++k)
        if (g_api_calls[k]) v.push_back({g_api_calls[k], k});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    g_api_timed = mode == 4;
    for (auto& [n, k] : v) {
        char b[96];
        if (mode == 4) snprintf(b, sizeof b, " %s %.1f (%.2f ms)", g_api_name[k], n / frames, g_api_ns[k] / 1e6 / frames);
        else snprintf(b, sizeof b, " %s %.1f", g_api_name[k], n / frames);
        line += b;
        g_api_calls[k] = 0;
        g_api_ns[k] = 0;
    }
    Log("api per frame:%s", line.c_str());
    if (mode == 3) {
        if (g_red_on) {
            std::string r;
            for (int i = 0; i < 10; ++i) {
                char b[48];
                snprintf(b, sizeof b, " %s %.0f/%.0f", kRedName[i], g_red[i] / frames, g_red_all[i] / frames);
                r += b;
                g_red[i] = g_red_all[i] = 0;
            }
            Log("api redundant per frame (same value as set):%s", r.c_str());
        }
        g_red_on = true;
    }
    if (mode == 3) {  // record the next whole frame (InstNote logs it at Present)
        g_inst_on = true; g_inst_armed = false;
    }
    if (mode == 2) { g_api_trace = true; g_trace_draws = 0; }
}

namespace {

// Builds the guest vtables (one thunk per method) and registers Direct3DCreate9.
struct Registrar {
    Registrar() {
        static const ImportDef create[] = {{"d3d9.dll", "Direct3DCreate9", x_Direct3DCreate9}};
        RegisterImports(create, 1);
        for (const InterfaceDef& i : kInterfaces) {
            std::vector<ImportDef> defs;
            const bool count = std::string(i.name) == "IDirect3DDevice9";  // always: settings are not read yet
            for (size_t k = 0; k < i.count; ++k) {
                std::string n = std::string(i.name) + "::" + i.methods[k].name;
                ImportFn fn = i.methods[k].fn;
                if (count && k < kApiSlots) { g_api_fn[k] = fn; g_api_name[k] = i.methods[k].name; fn = g_api_counted[k]; }
                defs.push_back({"d3d9.com", strdup(n.c_str()), fn});
            }
            RegisterImports(defs.data(), defs.size());
        }
    }
} g_registrar;

}  // namespace

// Called once guest memory exists (the vtables live there).
void InitD3D9Bridge() {
    for (const InterfaceDef& i : kInterfaces) {
        uint32_t vt = HeapAlloc(uint32_t(4 * i.count));
        for (size_t k = 0; k < i.count; ++k) {
            std::string n = std::string(i.name) + "::" + i.methods[k].name;
            W32(vt + uint32_t(4 * k), ImportThunk("d3d9.com", n.c_str()));
        }
        g_vtables[i.name] = vt;
    }
}

}  // namespace native

namespace native {
// Debug console `tex ADDR`: level 0 of a texture the game created (size, format, how much of it is non-zero).
std::string DebugTexture(uint32_t guest) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    uint32_t index = R32(guest + 4);
    if (!guest || !g_hosts.Valid(index)) return "not a D3D object\n";
    auto* base = static_cast<IDirect3DBaseTexture9*>(g_hosts.Get(index));
    IDirect3DTexture9* tex = nullptr;
    if (FAILED(base->QueryInterface(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&tex))) || !tex) return "not a 2D texture\n";
    tex->Release();
    D3DSURFACE_DESC d;
    tex->GetLevelDesc(0, &d);
    D3DLOCKED_RECT lr;
    char b[256];
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) {
        snprintf(b, sizeof b, "%ux%u fmt %x pool %d: LockRect failed\n", d.Width, d.Height, int(d.Format), int(d.Pool));
        return b;
    }
    uint32_t rows = d.Height;
    bool dxt = d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT3 || d.Format == D3DFMT_DXT5;
    if (dxt) rows = (d.Height + 3) / 4;
    size_t n = size_t(lr.Pitch) * rows, nz = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t v = static_cast<uint8_t*>(lr.pBits)[i];
        nz += v != 0;
        sum += v;
    }
    tex->UnlockRect(0);
    snprintf(b, sizeof b, "%ux%u fmt %x pool %d usage %x levels %u pitch %d: %zu of %zu bytes non-zero, mean %.1f\n", d.Width, d.Height,
             int(d.Format), int(d.Pool), d.Usage, tex->GetLevelCount(), int(lr.Pitch), nz, n, n ? double(sum) / double(n) : 0.0);
    return b;
}
}  // namespace native
