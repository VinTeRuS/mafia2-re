// Imports of mafia2.exe that nothing else in the host implements: the Steam wrapper (common.dll: GOG builds run without
// Steam), the PhysX/APEX loader glue (the real PhysXCore/APEX run as recompiled images), the waveOut/ACM audio imports,
// small Win32 leftovers, and native replacements of hot game functions (Native_<va>, NATIVE_OVERRIDES in
// tools/recomp/*.py). Anything not listed here still logs and stops when called (imports.cpp).
#include "m2_cull.h"
#include "m2_octree.h"
#include <cmath>
#include <mutex>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "win.h"

namespace native {

void ClientSize(int* w, int* h);  // user32.cpp

namespace {

// ---- helpers: a stdcall import with N argument dwords that just returns V ----
template <int N, uint32_t V>
void Ret(Ctx* c) { Return(c, V, N); }

// ---- common.dll: Steam wrapper. SteamAPI_* are cdecl; the interface getters return an object whose vtable slots are
// all "return 0" (thiscall methods, callee-pop unknown: cdecl-style return is right for the C++ interface calls the
// game makes through ecx when the method takes no arguments; refine when a call shows otherwise). ----
void s_Zero(Ctx* c) { Return(c, 0, 0); }
void s_One(Ctx* c) { Return(c, 1, 0); }

// Slot argument counts of GOG's common.dll interfaces (read from its vtables: `ret N` of each method; thiscall methods pop
// their own arguments, so a stub must pop exactly as many dwords). Every method returns 0, except where noted.
struct SteamIface {
    const char* name;
    std::vector<int> slots;
    std::vector<int> true_slots;  // methods answering 1
};
const std::vector<SteamIface>& SteamIfaces() {
    static const std::vector<SteamIface> v = {
        {"SteamUser", {0, 0, 1, 7, 2, 4, 2, 0, 0, 2, 8, 5, 3, 4, 2, 1, 3}, {}},
        {"SteamFriends", {0, 1, 0, 1, 3, 2, 2, 2, 3, 3, 3, 3, 0, 2, 2, 2, 4, 4, 3, 1, 3, 1, 1}, {}},
        {"SteamUtils", {0, 0, 0, 0, 0, 3, 3, 2, 0, 0, 1, 3, 2, 6, 0, 0, 1, 0, 0}, {}},
        {"SteamApps", {0, 0, 0, 0, 0, 0, 1, 1, 1}, {0, 6, 7, 8}},
        {"SteamUserStats", {0, 2, 2, 2, 2, 4, 2, 1, 1, 3, 0, 1, 2, 3, 2, 4, 4, 4, 5, 1, 3, 1, 2, 2, 2, 2, 5, 6, 6, 0}, {}},
        {"SteamNetworking", {5, 1, 4, 2, 2, 3, 4, 5, 3, 2, 2, 4, 2, 4, 3, 5, 5, 3, 1, 1}, {}},
        {"SteamClient", {0, 1, 1, 2, 2, 3, 3, 2, 3, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 1}, {}},
        {"SteamMatchmaking", {0, 7, 6, 5, 0, 3, 3, 2, 1, 1, 1, 2, 2, 2, 2, 4, 2, 4, 3, 4, 2, 7, 3, 5, 4, 4, 7, 2, 6, 5, 3, 2, 3, 3, 3, 4}, {}},
        {"SteamMatchmakingServers", {4, 2, 4, 4, 4, 4, 1, 2, 1, 1, 1, 1, 2, 3, 3, 3, 1, 1}, {}},
        {"SteamRemoteStorage", {3, 1, 3, 1, 0, 2, 2, 1}, {}},
    };
    return v;
}
template <int N, uint32_t V>
void SteamMethod(Ctx* c) { Return(c, V, N); }
using SteamFn = void (*)(Ctx*);
template <int N>
struct SteamTable {
    static constexpr SteamFn kZero = SteamMethod<N, 0>;
    static constexpr SteamFn kOne = SteamMethod<N, 1>;
};
// "ISteam::M<N>" / "ISteam::T<N>" import thunks (N = 0..8 argument dwords)
template <int... Ns>
void RegisterSteamMethods(std::integer_sequence<int, Ns...>) {
    static std::vector<std::string> names;
    static std::vector<ImportDef> defs;
    names.reserve(2 * sizeof...(Ns));
    ((names.push_back("ISteam::M" + std::to_string(Ns)), names.push_back("ISteam::T" + std::to_string(Ns))), ...);
    int k = 0;
    ((defs.push_back({"common.dll", names[k++].c_str(), SteamTable<Ns>::kZero}),
      defs.push_back({"common.dll", names[k++].c_str(), SteamTable<Ns>::kOne})), ...);
    RegisterImports(defs.data(), defs.size());
}
struct SteamMethodsInit {
    SteamMethodsInit() { RegisterSteamMethods(std::make_integer_sequence<int, 9>()); }
} g_steam_methods_init;

uint32_t SteamObject(const char* name) {
    static std::map<std::string, uint32_t> objects;
    auto it = objects.find(name);
    if (it != objects.end()) return it->second;
    for (const SteamIface& i : SteamIfaces()) {
        if (strcmp(i.name, name) != 0) continue;
        uint32_t vt = HeapAlloc(4 * uint32_t(i.slots.size() + 1));
        for (size_t k = 0; k < i.slots.size(); ++k) {
            bool yes = std::find(i.true_slots.begin(), i.true_slots.end(), int(k)) != i.true_slots.end();
            std::string method = std::string(yes ? "ISteam::T" : "ISteam::M") + std::to_string(i.slots[k]);
            W32(vt + 4 * uint32_t(k), ImportThunk("common.dll", method.c_str()));
        }
        uint32_t obj = HeapAlloc(8);
        W32(obj, vt);
        return objects[name] = obj;
    }
    return 0;
}
template <int I>
void s_IfaceN(Ctx* c) { Return(c, SteamObject(SteamIfaces()[I].name), 0); }

// ---- PhysXLoader.dll: what the loader does. The real PhysXCore.dll / NxCooking.dll run as recompiled images; each Nx*
// export forwards its stack arguments to the core's Np* export (cdecl). APEX_release.dll is not available yet. ----
template <int N>
void PxForward(Ctx* c, const char* dll, const char* name) {
    uint32_t fn = ExtraDllExport(dll, name);
    if (!fn) {
        Log("PhysX: %s!%s not found", dll, name);
        Return(c, 0, 0);
        return;
    }
    uint32_t args[N ? N : 1] = {};
    for (int i = 0; i < N; ++i) args[i] = Arg(c, i);
    uint32_t r;
    if constexpr (N == 0) r = GuestCall(fn, {});
    else if constexpr (N == 1) r = GuestCall(fn, {args[0]});
    else if constexpr (N == 2) r = GuestCall(fn, {args[0], args[1]});
    else if constexpr (N == 3) r = GuestCall(fn, {args[0], args[1], args[2]});
    else if constexpr (N == 4) r = GuestCall(fn, {args[0], args[1], args[2], args[3]});
    else r = GuestCall(fn, {args[0], args[1], args[2], args[3], args[4]});
    Return(c, r, 0);
}
// An NxUserOutputStream of our own for the SDK when the game passes none: PhysX reports errors/asserts/prints into the log.
void ps_reportError(Ctx* c) {  // thiscall (this ecx, code, message, file, line): ret 0x10
    Log("PhysX error %u: %s (%s:%d)", Arg(c, 0), Str(Arg(c, 1)), Arg(c, 2) ? Str(Arg(c, 2)) : "?", int(Arg(c, 3)));
    Return(c, 0, 4);
}
void ps_assert(Ctx* c) {  // reportAssertViolation(message, file, line) -> NX_AR_CONTINUE (0)
    Log("PhysX assert: %s (%s:%d)", Str(Arg(c, 0)), Arg(c, 1) ? Str(Arg(c, 1)) : "?", int(Arg(c, 2)));
    Return(c, 0, 3);
}
void ps_print(Ctx* c) {
    Log("PhysX: %s", Str(Arg(c, 0)));
    Return(c, 0, 1);
}
void ps_dtor(Ctx* c) { Return(c, 0, 1); }
uint32_t PhysxOutputStream() {
    static uint32_t obj = 0;
    if (!obj) {
        uint32_t vt = HeapAlloc(16);
        W32(vt + 0, ImportThunk("physxloader.dll", "NxUserOutputStream::reportError"));
        W32(vt + 4, ImportThunk("physxloader.dll", "NxUserOutputStream::reportAssertViolation"));
        W32(vt + 8, ImportThunk("physxloader.dll", "NxUserOutputStream::print"));
        W32(vt + 12, ImportThunk("physxloader.dll", "NxUserOutputStream::dtor"));
        obj = HeapAlloc(8);
        W32(obj, vt);
    }
    return obj;
}

void px_NxCreatePhysicsSDK(Ctx* c) {
    // The core writes *errorCode unconditionally; the loader hands it a local when the game passes NULL.
    static uint32_t scratch = HeapAlloc(16);
    if (!Arg(c, 4)) W32(c->esp + 4 + 4 * 4, scratch);
    if (!Arg(c, 2)) W32(c->esp + 4 + 4 * 2, PhysxOutputStream());
    PxForward<5>(c, "physxcore.dll", "NpCreatePhysicsSDK");
}
void px_NxGetPhysicsSDK(Ctx* c) { PxForward<0>(c, "physxcore.dll", "NpGetPhysicsSDK"); }
void px_NxGetFoundationSDK(Ctx* c) { PxForward<0>(c, "physxcore.dll", "NpGetFoundationSDK"); }
void px_NxGetUtilLib(Ctx* c) { PxForward<0>(c, "physxcore.dll", "NpGetUtilLib"); }
void px_NxGetCookingLib(Ctx* c) { PxForward<1>(c, "nxcooking.dll", "NxGetCookingInterface"); }
void px_Null(Ctx* c) {  // NxGetApexSDK()
    Log("APEX entry point called: not implemented (returning NULL)");
    Return(c, 0, 0);
}
void px_NxCreateApexSDK(Ctx* c) {  // (desc, errorCode*): stdcall, 2 argument dwords
    Log("NxCreateApexSDK: not implemented (returning NULL)");
    if (Arg(c, 1)) W32(Arg(c, 1), 1);
    Return(c, 0, 2);
}

// ---- GetCommandLineW ----
void k_GetCommandLineW(Ctx* c) {
    static uint32_t line = 0;
    if (!line) {
        const char16_t text[] = u"\"C:\\Game\\mafia2.exe\"";
        line = HeapAlloc(sizeof(text));
        for (size_t i = 0; i < sizeof(text) / 2; ++i) W16(line + 2 * uint32_t(i), text[i]);
    }
    Return(c, line, 0);
}


// ---- kernel32 / user32 / shell32 / oleaut32 leftovers ----
template <int N, uint32_t ERR>
void RetNull(Ctx* c) { SetLastError(ERR); Return(c, 0, N); }

void k_lstrcmpiW(Ctx* c) {
    std::string a = WideToUtf8(Arg(c, 0)), b = WideToUtf8(Arg(c, 1));
    for (char& ch : a) ch = char(tolower((unsigned char)ch));
    for (char& ch : b) ch = char(tolower((unsigned char)ch));
    Return(c, a < b ? uint32_t(-1) : (a == b ? 0u : 1u), 2);
}
void k_GetModuleHandleExA(Ctx* c) {  // (flags, name, HMODULE* out): the exe, whatever was asked
    uint32_t out = Arg(c, 2);
    if (out) W32(out, g_engine.base);
    Return(c, 1, 3);
}
void sh_SHGetFolderPathA(Ctx* c) {  // (hwnd, csidl, token, flags, char* path)
    const char* dir = "C:\\Game\\userdata";
    uint32_t buf = Arg(c, 4);
    for (size_t i = 0;; ++i) { W8(buf + uint32_t(i), uint8_t(dir[i])); if (!dir[i]) break; }
    Return(c, 0, 5);
}
void o_SysAllocString(Ctx* c) {  // BSTR: 4-byte byte length, UTF-16 data, NUL
    uint32_t src = Arg(c, 0);
    if (!src) { Return(c, 0, 1); return; }
    uint32_t n = 0;
    while (R16(src + 2 * n)) ++n;
    uint32_t block = HeapAlloc(4 + 2 * (n + 1));
    W32(block, 2 * n);
    for (uint32_t i = 0; i <= n; ++i) W16(block + 4 + 2 * i, R16(src + 2 * i));
    Return(c, block + 4, 1);
}
void o_SysFreeString(Ctx* c) { Return(c, 0, 1); }

// EnumDisplaySettingsA(device, mode, DEVMODEA*): ENUM_CURRENT_SETTINGS (-1) is the window; the list is a few 32-bit modes.
void u_EnumDisplaySettingsA(Ctx* c) {
    static const int kModes[][2] = {{800, 600}, {1024, 768}, {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2400, 1080}};
    constexpr int kCount = sizeof(kModes) / sizeof(kModes[0]);
    int32_t mode = int32_t(Arg(c, 1));
    uint32_t dm = Arg(c, 2);
    int w, h;
    ClientSize(&w, &h);
    if (mode >= kCount || mode < -2) { Return(c, 0, 3); return; }
    if (mode >= 0) { w = kModes[mode][0]; h = kModes[mode][1]; }
    for (uint32_t i = 0; i < 156; ++i) W8(dm + i, 0);
    W16(dm + 36, 156);                 // dmSize
    W32(dm + 40, 0x5c0000u | 0x20000u); // dmFields: BITSPERPEL | PELSWIDTH | PELSHEIGHT | DISPLAYFREQUENCY
    W32(dm + 104, 32);                 // dmBitsPerPel
    W32(dm + 108, uint32_t(w));        // dmPelsWidth
    W32(dm + 112, uint32_t(h));        // dmPelsHeight
    W32(dm + 120, 60);                 // dmDisplayFrequency
    Return(c, 1, 3);
}

// ---- version.dll: only the PhysX System Software check (the game wants PhysXCore.dll >= 2.8.3 at the registry path) ----
bool IsPhysxCore(const std::string& path) {
    std::string l = path;
    for (char& ch : l) ch = char(tolower((unsigned char)ch));
    return l.size() >= 13 && l.compare(l.size() - 13, 13, "physxcore.dll") == 0;
}
void v_GetFileVersionInfoSizeA(Ctx* c) {
    if (Arg(c, 1)) W32(Arg(c, 1), 0);
    if (IsPhysxCore(Str(Arg(c, 0)))) { Return(c, 0x100, 2); return; }
    SetLastError(1813);
    Return(c, 0, 2);
}
void v_GetFileVersionInfoA(Ctx* c) {  // (file, handle, len, data): VS_FIXEDFILEINFO at +0x20
    if (!IsPhysxCore(Str(Arg(c, 0)))) { SetLastError(1813); Return(c, 0, 4); return; }
    uint32_t d = Arg(c, 3);
    for (uint32_t i = 0; i < 0x100; ++i) W8(d + i, 0);
    uint32_t f = d + 0x20;
    W32(f, 0xFEEF04BDu); W32(f + 4, 0x10000);
    W32(f + 8, 0x00020008); W32(f + 12, 0x00030025);   // FileVersion 2.8.3.37
    W32(f + 16, 0x00020008); W32(f + 20, 0x00030025);  // ProductVersion
    Return(c, 1, 4);
}
void v_VerQueryValueA(Ctx* c) {  // (block, subBlock, void** out, UINT* len)
    std::string sub = Str(Arg(c, 1));
    if (sub == "\\") {
        W32(Arg(c, 2), Arg(c, 0) + 0x20);
        if (Arg(c, 3)) W32(Arg(c, 3), 52);
        Return(c, 1, 4);
        return;
    }
    Return(c, 0, 4);
}

// ---- cudart32_30_9.dll (PhysXCore's GPU path; there is no CUDA device): registration calls succeed, everything that
// would use a device fails with cudaErrorNoDevice (38) so PhysX stays on the CPU. stdcall (CUDARTAPI). ----
void cu_RegisterFatBinary(Ctx* c) {
    static uint32_t handle = 0;
    if (!handle) handle = HeapAlloc(16);
    Return(c, handle, 1);
}
void cu_CreateChannelDesc(Ctx* c) {  // hidden struct return pointer + (x, y, z, w, kind)
    uint32_t out = Arg(c, 0);
    for (int i = 0; i < 5; ++i) W32(out + 4 * uint32_t(i), Arg(c, 1 + i));
    Return(c, out, 6);
}
void cu_GetDeviceCount(Ctx* c) { if (Arg(c, 0)) W32(Arg(c, 0), 0); Return(c, 38, 1); }

// ---- wsock32 by name (PhysXCore's remote debugger connection: offline) ----
void ws_htons(Ctx* c) { uint32_t v = Arg(c, 0) & 0xffff; Return(c, ((v & 0xff) << 8) | (v >> 8), 1); }

// ---- dsound.dll odds and ends (dsound.cpp has DirectSoundCreate*) ----
void ds_DirectSoundEnumerateA(Ctx* c) {  // (callback, context): one device, the primary driver
    uint32_t cb = Arg(c, 0), ctx = Arg(c, 1);
    // Windows lists the primary driver with a NULL GUID, then each device with its own. FMOD skips the NULL entry, so
    // with that alone it finds no driver and runs silent.
    static uint32_t desc = GuestStrdup("Primary Sound Driver"), speakers = GuestStrdup("Speakers"), module = GuestStrdup("");
    static uint32_t guid = [] {
        uint32_t g = HeapAlloc(16);
        memcpy(G(g), "M2SOUNDDEVICE001", 16);
        return g;
    }();
    if (cb && GuestCall(cb, {0, desc, module, ctx})) GuestCall(cb, {guid, speakers, module, ctx});
    Return(c, 0, 2);
}

const ImportDef kM2[] = {
    // Steam wrapper (cdecl)
    {"common.dll", "SteamAPI_Init", s_One},
    {"common.dll", "SteamAPI_Shutdown", s_Zero},
    {"common.dll", "SteamAPI_RunCallbacks", s_Zero},
    {"common.dll", "SteamAPI_RegisterCallback", s_Zero},
    {"common.dll", "SteamAPI_UnregisterCallback", s_Zero},
    {"common.dll", "SteamAPI_RegisterCallResult", s_Zero},
    {"common.dll", "SteamAPI_UnregisterCallResult", s_Zero},
    {"common.dll", "SteamAPI_RestartAppIfNecessary", s_Zero},
    {"common.dll", "SteamUser", s_IfaceN<0>},
    {"common.dll", "SteamFriends", s_IfaceN<1>},
    {"common.dll", "SteamUtils", s_IfaceN<2>},
    {"common.dll", "SteamApps", s_IfaceN<3>},
    {"common.dll", "SteamUserStats", s_IfaceN<4>},
    {"common.dll", "SteamNetworking", s_IfaceN<5>},
    {"common.dll", "SteamClient", s_IfaceN<6>},
    {"common.dll", "SteamMatchmaking", s_IfaceN<7>},
    {"common.dll", "SteamMatchmakingServers", s_IfaceN<8>},
    {"common.dll", "SteamRemoteStorage", s_IfaceN<9>},
    {"common.dll", "ISteam::Method", s_Zero},
    // PhysX / APEX
    {"physxloader.dll", "NxGetFoundationSDK", px_NxGetFoundationSDK}, {"physxloader.dll", "NxGetUtilLib", px_NxGetUtilLib},
    {"physxloader.dll", "NxGetPhysicsSDK", px_NxGetPhysicsSDK}, {"physxloader.dll", "NxGetCookingLib", px_NxGetCookingLib},
    {"physxloader.dll", "NxCreatePhysicsSDK", px_NxCreatePhysicsSDK},
    {"physxloader.dll", "NxUserOutputStream::reportError", ps_reportError}, {"physxloader.dll", "NxUserOutputStream::reportAssertViolation", ps_assert},
    {"physxloader.dll", "NxUserOutputStream::print", ps_print}, {"physxloader.dll", "NxUserOutputStream::dtor", ps_dtor},
    {"apex_release.dll", "NxCreateApexSDK", px_NxCreateApexSDK}, {"apex_release.dll", "NxGetApexSDK", px_Null},
    // winmm: no wave devices yet (MMSYSERR_NODRIVER = 6), timers never fire
    {"winmm.dll", "timeSetEvent", Ret<5, 1>}, {"winmm.dll", "timeKillEvent", Ret<1, 0>},
    {"winmm.dll", "waveInGetNumDevs", Ret<0, 0>}, {"winmm.dll", "waveOutGetNumDevs", Ret<0, 0>},
    {"winmm.dll", "waveOutGetDevCapsA", Ret<3, 6>}, {"winmm.dll", "waveOutOpen", Ret<6, 6>},
    {"winmm.dll", "waveOutClose", Ret<1, 0>}, {"winmm.dll", "waveOutReset", Ret<1, 0>},
    {"winmm.dll", "waveOutWrite", Ret<3, 0>}, {"winmm.dll", "waveOutPrepareHeader", Ret<3, 0>},
    {"winmm.dll", "waveOutUnprepareHeader", Ret<3, 0>}, {"winmm.dll", "waveOutGetPosition", Ret<3, 0>},
    // msacm32: MMSYSERR_NODRIVER
    {"msacm32.dll", "acmStreamOpen", Ret<8, 6>}, {"msacm32.dll", "acmStreamSize", Ret<4, 6>},
    {"msacm32.dll", "acmStreamPrepareHeader", Ret<3, 6>}, {"msacm32.dll", "acmStreamUnprepareHeader", Ret<3, 6>},
    {"msacm32.dll", "acmStreamConvert", Ret<3, 6>}, {"msacm32.dll", "acmFormatSuggest", Ret<5, 6>},
    // small Win32 leftovers
    {"wtsapi32.dll", "WTSRegisterSessionNotification", Ret<2, 1>},
    {"wtsapi32.dll", "WTSUnRegisterSessionNotification", Ret<1, 1>},
    {"userenv.dll", "CreateEnvironmentBlock", Ret<3, 0>}, {"userenv.dll", "DestroyEnvironmentBlock", Ret<1, 1>},
    {"xinput1_3.dll", "#5", Ret<1, 0>},
    {"kernel32.dll", "SetConsoleCtrlHandler", Ret<2, 1>}, {"kernel32.dll", "SwitchToThread", Ret<0, 1>},
    {"kernel32.dll", "GetCommandLineW", k_GetCommandLineW},
    {"kernel32.dll", "GetConsoleWindow", Ret<0, 0>}, {"kernel32.dll", "GetConsoleOutputCP", Ret<0, 437>},
    {"advapi32.dll", "InitializeSecurityDescriptor", Ret<2, 1>}, {"advapi32.dll", "SetSecurityDescriptorDacl", Ret<4, 1>},
    {"advapi32.dll", "OpenProcessToken", Ret<3, 0>},
    {"user32.dll", "GetParent", Ret<1, 0>},
    {"dsound.dll", "DirectSoundEnumerateA", ds_DirectSoundEnumerateA},
    {"dsound.dll", "DirectSoundCaptureCreate8", Ret<3, 0x88780078u>}, {"dsound.dll", "DirectSoundCaptureCreate", Ret<3, 0x88780078u>},
    {"dsound.dll", "DirectSoundCaptureEnumerateA", Ret<2, 0>}, {"user32.dll", "MessageBoxW", Ret<4, 1>}, {"user32.dll", "EnumDisplayDevicesA", Ret<4, 0>},
    {"cudart32_30_9.dll", "__cudaRegisterFatBinary", cu_RegisterFatBinary}, {"cudart32_30_9.dll", "cudaCreateChannelDesc", cu_CreateChannelDesc},
    {"cudart32_30_9.dll", "cudaGetLastError", Ret<0, 0>}, {"cudart32_30_9.dll", "cudaGetDeviceCount", cu_GetDeviceCount},
    {"cudart32_30_9.dll", "cudaBindTexture", Ret<5, 38>},
    {"cudart32_30_9.dll", "cudaUnbindTexture", Ret<1, 38>},
    {"cudart32_30_9.dll", "cudaMemcpyToSymbolAsync", Ret<6, 38>},
    {"cudart32_30_9.dll", "__cudaRegisterFunction", Ret<10, 0>},
    {"cudart32_30_9.dll", "__cudaRegisterShared", Ret<2, 0>},
    {"cudart32_30_9.dll", "__cudaRegisterTexture", Ret<7, 0>},
    {"cudart32_30_9.dll", "__cudaRegisterVar", Ret<8, 0>},
    {"cudart32_30_9.dll", "cudaSetupArgument", Ret<3, 38>},
    {"cudart32_30_9.dll", "cudaLaunch", Ret<1, 38>},
    {"cudart32_30_9.dll", "__cudaUnregisterFatBinary", Ret<1, 0>},
    {"cudart32_30_9.dll", "cudaMemset", Ret<3, 38>},
    {"cudart32_30_9.dll", "cudaMallocHost", Ret<2, 38>},
    {"cudart32_30_9.dll", "cudaMalloc", Ret<2, 38>},
    {"cudart32_30_9.dll", "cudaFreeHost", Ret<1, 38>},
    {"cudart32_30_9.dll", "cudaFree", Ret<1, 38>},
    {"cudart32_30_9.dll", "cudaEventSynchronize", Ret<1, 38>},
    {"cudart32_30_9.dll", "cudaEventElapsedTime", Ret<3, 38>},
    {"cudart32_30_9.dll", "cudaEventQuery", Ret<1, 38>},
    {"cudart32_30_9.dll", "cudaEventRecord", Ret<2, 38>},
    {"cudart32_30_9.dll", "cudaEventCreate", Ret<1, 38>},
    {"cudart32_30_9.dll", "cudaGetDeviceProperties", Ret<2, 38>},
    {"cudart32_30_9.dll", "cudaMemcpyAsync", Ret<5, 38>},
    {"cudart32_30_9.dll", "cudaThreadSynchronize", Ret<0, 38>},
    {"cudart32_30_9.dll", "cudaMemcpy", Ret<4, 38>},
    {"cudart32_30_9.dll", "cudaConfigureCall", Ret<8, 38>},
    {"wsock32.dll", "WSAStartup", Ret<2, 10091>}, {"wsock32.dll", "WSAGetLastError", Ret<0, 10061>},
    {"wsock32.dll", "socket", Ret<3, 0xffffffffu>}, {"wsock32.dll", "closesocket", Ret<1, 0xffffffffu>},
    {"wsock32.dll", "connect", Ret<3, 0xffffffffu>}, {"wsock32.dll", "recv", Ret<4, 0xffffffffu>},
    {"wsock32.dll", "send", Ret<4, 0xffffffffu>}, {"wsock32.dll", "ioctlsocket", Ret<3, 0xffffffffu>},
    {"wsock32.dll", "gethostbyname", Ret<1, 0>}, {"wsock32.dll", "gethostbyaddr", Ret<3, 0>},
    {"wsock32.dll", "inet_addr", Ret<1, 0xffffffffu>}, {"wsock32.dll", "htons", ws_htons},
    {"kernel32.dll", "HeapValidate", Ret<3, 1>}, {"kernel32.dll", "HeapWalk", Ret<2, 0>},
    {"version.dll", "GetFileVersionInfoSizeA", v_GetFileVersionInfoSizeA}, {"version.dll", "GetFileVersionInfoA", v_GetFileVersionInfoA},
    {"version.dll", "VerQueryValueA", v_VerQueryValueA},
    {"kernel32.dll", "OpenMutexA", RetNull<3, 2>}, {"kernel32.dll", "OpenEventA", RetNull<3, 2>},
    {"kernel32.dll", "OpenFileMappingA", RetNull<3, 2>}, {"kernel32.dll", "CreateFileMappingA", RetNull<6, 5>},
    {"kernel32.dll", "FatalAppExitA", Ret<2, 0>}, {"kernel32.dll", "WriteConsoleA", Ret<5, 1>},
    {"kernel32.dll", "CreatePipe", RetNull<4, 5>}, {"kernel32.dll", "CreateProcessA", RetNull<10, 5>},
    {"kernel32.dll", "CreateProcessW", RetNull<10, 5>}, {"kernel32.dll", "SetFileTime", Ret<4, 1>},
    {"kernel32.dll", "SetWaitableTimer", Ret<6, 1>}, {"kernel32.dll", "CreateWaitableTimerA", RetNull<3, 5>},
    {"kernel32.dll", "GetExitCodeProcess", Ret<2, 0>}, {"kernel32.dll", "SetEnvironmentVariableW", Ret<2, 1>},
    {"kernel32.dll", "SystemTimeToTzSpecificLocalTime", Ret<3, 0>}, {"kernel32.dll", "SearchPathA", RetNull<6, 2>},
    {"kernel32.dll", "LoadResource", RetNull<2, 1813>}, {"kernel32.dll", "LockResource", Ret<1, 0>},
    {"kernel32.dll", "SizeofResource", RetNull<2, 1813>}, {"kernel32.dll", "FindResourceW", RetNull<3, 1813>},
    {"kernel32.dll", "FindResourceExA", RetNull<4, 1813>}, {"kernel32.dll", "GetModuleHandleExA", k_GetModuleHandleExA},
    {"kernel32.dll", "lstrcmpiW", k_lstrcmpiW},
    {"user32.dll", "wvsprintfW", Ret<3, 0>}, {"user32.dll", "IsDialogMessageA", Ret<2, 0>},
    {"user32.dll", "MsgWaitForMultipleObjects", Ret<5, 0x102>}, {"user32.dll", "RegisterWindowMessageA", Ret<1, 0xC001>},
    {"user32.dll", "SendInput", Ret<3, 0>}, {"user32.dll", "LoadBitmapA", Ret<2, 0>},
    {"user32.dll", "MonitorFromRect", Ret<2, 0x10001>}, {"user32.dll", "LoadStringA", Ret<4, 0>},
    {"gdi32.dll", "BitBlt", Ret<9, 1>},
    {"user32.dll", "EnumDisplaySettingsA", u_EnumDisplaySettingsA},
    {"advapi32.dll", "CryptSignHashA", Ret<6, 0>}, {"advapi32.dll", "CryptGetUserKey", Ret<3, 0>},
    {"advapi32.dll", "CryptExportKey", Ret<6, 0>},
    {"shell32.dll", "SHGetFolderPathA", sh_SHGetFolderPathA}, {"shell32.dll", "ShellExecuteA", Ret<6, 33>},
    {"ole32.dll", "CLSIDFromString", Ret<2, 0x80070057>}, {"ole32.dll", "PropVariantClear", Ret<1, 0>},
    {"ole32.dll", "CoInitializeEx", Ret<2, 0>}, {"ole32.dll", "CoTaskMemFree", Ret<1, 0>},
    {"ole32.dll", "CoSetProxyBlanket", Ret<8, 0>}, {"ole32.dll", "StringFromGUID2", Ret<3, 0>},
    {"oleaut32.dll", "SysAllocString", o_SysAllocString}, {"oleaut32.dll", "SysFreeString", o_SysFreeString},
};
ImportRegistrar g_m2(kM2);

}  // namespace

}  // namespace native

// ---- CRT memcpy/memmove of every recompiled image (their computed jump tables are not recompilable): host memmove ----
static bool g_memmove_stats = false;
static std::mutex g_memmove_mutex;
static std::map<uint32_t, std::pair<uint64_t, uint64_t>> g_memmove;  // return address -> calls, bytes
static void NativeMemmove(Ctx* c) {  // cdecl void* f(void* dst, const void* src, size_t n)
    uint32_t dst = native::Arg(c, 0), src = native::Arg(c, 1), n = native::Arg(c, 2);
    if (n) memmove(native::Ptr<void>(dst), native::Ptr<void>(src), n);
    if (g_memmove_stats) {
        std::lock_guard<std::mutex> lock(g_memmove_mutex);
        auto& st = g_memmove[R32(c->esp)];
        ++st.first;
        st.second += n;
    }
    native::Return(c, dst, 0);
}
namespace native {
// rt_stats: who copies how much through the CRT memcpy/memmove (bytes by return address, logged with the render stats).
void MemmoveStatsLog() {
    g_memmove_stats = true;
    std::lock_guard<std::mutex> lock(g_memmove_mutex);
    std::vector<std::pair<uint64_t, uint32_t>> top;
    uint64_t total = 0, calls = 0;
    for (auto& [ret, st] : g_memmove) { top.push_back({st.second, ret}); total += st.second; calls += st.first; }
    std::sort(top.rbegin(), top.rend());
    Log("memmove: %llu MB in %llu calls", (unsigned long long)(total >> 20), (unsigned long long)calls);
    for (size_t i = 0; i < top.size() && i < 8; ++i)
        Log("memmove from %08x: %llu MB, %llu calls", top[i].second, (unsigned long long)(top[i].first >> 20), (unsigned long long)g_memmove[top[i].second].first);
    g_memmove.clear();
}
}  // namespace native
// FMOD's MPEG decoder, polyphase synthesis (mpg123's synth_1to1): 36% of the mixer thread as recompiled x87.
// thiscall (this unused), stack: b0 (float*), bo1, step (shorts between samples), out (short*); ret 0x10.
static inline int16_t SynthClip(float v, double scale) {
    long r = lrint(double(v) * scale);
    return int16_t(r > 0x7fff ? 0x7fff : r < -0x8000 ? -0x8000 : r);
}
extern "C" void Native_00790817(Ctx* c) {
    const float* b = native::Ptr<float>(native::Arg(c, 0));
    int32_t bo1 = int32_t(native::Arg(c, 1));
    uint32_t step = native::Arg(c, 2);
    int16_t* out = native::Ptr<int16_t>(native::Arg(c, 3));
    const float* w = native::Ptr<float>(R32(0x1c25e98u)) + (16 - bo1);
    double scale;
    memcpy(&scale, G(0x18ff330u), 8);
    for (int j = 0; j < 16; ++j, w += 32, b += 16, out += step) {
        float s = 0;
        for (int k = 0; k < 16; k += 2) s += w[k] * b[k] - w[k + 1] * b[k + 1];
        *out = SynthClip(s, scale);
    }
    {
        float s = 0;
        for (int k = 0; k < 16; k += 2) s += w[k] * b[k];
        *out = SynthClip(s, scale);
        out += step;
    }
    w += 2 * bo1 - 32 - 2;
    b -= 16;
    for (int j = 0; j < 15; ++j, w -= 32, b -= 16, out += step) {
        float s = 0;
        s -= w[1] * b[0];
        for (int k = 1; k < 16; ++k) s -= w[1 - k] * b[k];
        *out = SynthClip(s, scale);
    }
    native::Return(c, 0, 4);
}
// Box against frustum planes (m2_cull.h; difftest tools/difftest/cull_test.sh): cdecl (min, max, planes, mask, out).
extern "C" void Native_01652180(Ctx* c) {
    uint32_t out = 0;
    uint32_t r = M2CullBox(native::Ptr<float>(native::Arg(c, 0)), native::Ptr<float>(native::Arg(c, 1)),
                           native::Ptr<float>(native::Arg(c, 2)), native::Arg(c, 3), &out);
    W32(native::Arg(c, 4), out);
    native::Return(c, r, 0);
}
// Octree query f_0066feb0 (m2_octree.h), GATE_HOOK: octree_native = 0 runs the recompiled original; octree_check = 1
// runs both (native without side effects first) and logs any difference in the objects written to the out array.
static thread_local bool tl_octree_inside;
extern "C" int Gate_OctreeQuery(Ctx* c) {
    static const int on = native::SettingInt("octree_native", 1);
    static const int check = native::SettingInt("octree_check", 0);
    if (!on || tl_octree_inside) return 0;
    const uint32_t tree = c->ecx, level = native::Arg(c, 0) & 0xff, root = native::Arg(c, 1), qbox = native::Arg(c, 2),
                   sink = native::Arg(c, 3), out = native::Arg(c, 4);
    if (check) {
        std::vector<uint32_t> want;
        m2oct::Walk(tree, uint8_t(level), root, qbox, [&](uint32_t o) { want.push_back(o); return true; });
        const uint32_t before = R32(out + 0x208), cap = R32(out + 0x204);
        tl_octree_inside = true;
        const uint32_t r = native::GuestCallFrom(c, 0x0066feb0, {native::Arg(c, 0), root, qbox, sink, out}, tree) & 0xff;
        tl_octree_inside = false;
        static uint64_t calls, objects, bad, skipped;
        ++calls;
        if (before + want.size() > cap) {
            ++skipped;  // the original flushed to the sink: only the uninterrupted case is compared
        } else {
            objects += want.size();
            bool same = r == 1 && R32(out + 0x208) == before + want.size();
            for (size_t i = 0; same && i < want.size(); ++i) same = R32(R32(out + 0x200) + 4 * (before + i)) == want[i];
            if (!same && ++bad <= 20)
                native::Log("octree check: MISMATCH tree %x level %u root %x: native %zu objects, original %u (r %u)", tree,
                            level, root, want.size(), R32(out + 0x208) - before, r);
        }
        if ((calls & 0xffff) == 0)
            native::Log("octree check: %llu calls, %llu objects compared, %llu mismatches, %llu flushed (not compared)",
                        (unsigned long long)calls, (unsigned long long)objects, (unsigned long long)bad,
                        (unsigned long long)skipped);
        native::Return(c, r, 5);
        return 1;
    }
    const bool r = m2oct::Walk(tree, uint8_t(level), root, qbox, [&](uint32_t o) {
        uint32_t n = R32(out + 0x208);
        const uint32_t cap = R32(out + 0x204);
        if (n == cap) {
            const uint32_t fn = R32(R32(sink) + 4);
            if ((native::GuestCallFrom(c, fn, {R32(out + 0x200), n}, sink) & 0xff) == 0) return false;
            W32(out + 0x208, 0);
            n = 0;
        }
        if (n < cap) {
            W32(R32(out + 0x200) + 4 * n, o);
            W32(out + 0x208, n + 1);
        }
        return true;
    });
    native::Return(c, r ? 1 : 0, 5);
    return 1;
}
// Box inside `count` planes (m2_cull.h): cdecl (min, max, planes, count).
extern "C" void Native_016522a0(Ctx* c) {
    native::Return(c, M2CullBoxAll(native::Ptr<float>(native::Arg(c, 0)), native::Ptr<float>(native::Arg(c, 1)),
                                   native::Ptr<float>(native::Arg(c, 2)), int32_t(native::Arg(c, 3))), 0);
}
extern "C" void Native_00e73200(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_00e75b50(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_101e19c0(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_101e1ea0(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10030350(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_100306c0(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_1002ee00(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10033b30(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10031e20(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10037180(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_1004aa10(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10058670(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_1002e090(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_1003a6e0(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10029710(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10035fc0(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10015d40(Ctx* c) { NativeMemmove(c); }
extern "C" void Native_10016340(Ctx* c) { NativeMemmove(c); }

// ---- PRE_HOOKS (tools/recomp/*.py): run at a recompiled function's entry ----
// The game's own video options (menu "Графика"), stored in the player profile: thiscall setters with one argument,
// called when the profile loads and from the menu. Settings game_geometry (0-2: LOD distance x1.0/x1.3/x1.6),
// game_shadows (0-2) and game_ao (0/1) force a value; -1 keeps the player's choice (desktop default). Android defaults
// to the lowest of all three: the GPU is the limit on phones (low has about a fifth fewer render passes than high).
// game_detail (the settings panel's "Графика игры": 0 low, 1 medium, 2 high) sets all three; a game_* key wins over it.
static void ForceVideoOption(Ctx* c, const char* key, const int (&by_detail)[3], const char* what) {
#ifdef __ANDROID__
    const int detail = native::SettingInt("game_detail", 0);
#else
    const int detail = native::SettingInt("game_detail", -1);
#endif
    const int v = native::SettingInt(key, detail >= 0 && detail <= 2 ? by_detail[detail] : -1);
    if (v < 0) return;
    const uint32_t old = R32(c->esp + 4);
    if (old != uint32_t(v)) native::Log("video option %s: %u -> %d (setting %s)", what, old, v, key);
    W32(c->esp + 4, uint32_t(v));
}
extern "C" void Hook_SetAmbientOcclusion(Ctx* c) { ForceVideoOption(c, "game_ao", {0, 0, 1}, "ambient occlusion"); }
extern "C" void Hook_SetShadowQuality(Ctx* c) { ForceVideoOption(c, "game_shadows", {0, 1, 2}, "shadow quality"); }
extern "C" void Hook_SetGeometryDetail(Ctx* c) { ForceVideoOption(c, "game_geometry", {0, 1, 2}, "geometry detail"); }

// f_0173db90 (renderer setup, thiscall): `if (device->gpu_count > 1) flags &= ~0x0c` switches off the game's shadow
// map cache for SLI/CrossFire. The count (device + 0x2040) comes from NvAPI or, for an ATI adapter, from a helper
// (f_00ead670) that leaves it uninitialised here, so the game saw several GPUs and redrew its three far shadow maps
// (50, 125 and 312 m: ~1 100 of ~2 900 draws a frame) every frame. With one GPU a far map is redrawn only when the
// camera has moved a tenth of the map's range from where it was drawn; objects that appear later are added to it.
// Setting shadow_cache = 0 gives the old behaviour.
extern "C" void Hook_RendererSetup(Ctx* c) {
    static const bool cache = native::SettingInt("shadow_cache", 1) != 0;
    if (!cache) return;
    const uint32_t device = R32(c->ecx + 0xa4);
    if (!device) return;
    const uint32_t gpus = R32(device + 0x2040);
    if (gpus != 1) native::Log("renderer: GPU count %u -> 1 (keeps the game's shadow map cache on)", gpus);
    W32(device + 0x2040, 1);
}
// Cloth::solve (f_10005230, thiscall): the cost is linear in solverIterations ([this+0x80], default 5). Setting
// cloth_iterations (0 = the game's value) caps it, like NpCloth::setSolverIterations would.
extern "C" void Hook_ClothSolve(Ctx* c) {
    static const int cap = native::SettingInt("cloth_iterations", 0);
    if (cap <= 0 || !c->ecx) return;
    if (R32(c->ecx + 0x80) > uint32_t(cap)) W32(c->ecx + 0x80, uint32_t(cap));
}
// NxScene::createCloth(const NxClothDesc& desc): safety net against NaN poses (once caused by the recompiler dropping
// flags at ret). A NaN globalPose makes PhysX reject the cloth, so it is replaced by identity and
// logged; it has not fired since that fix.
extern "C" void Hook_PxCreateCloth(Ctx* c) {
    uint32_t desc = R32(c->esp + 4);
    if (!desc) return;
    bool nan = false;
    for (int i = 0; i < 12; ++i) {
        uint32_t bits = R32(desc + 4 + 4 * uint32_t(i));
        if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu)) nan = true;
    }
    if (!nan) return;
    static const uint32_t kIdentity[12] = {0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0, 0x3f800000, 0, 0, 0};  // M (3x3 row-major), t
    for (int i = 0; i < 12; ++i) W32(desc + 4 + 4 * uint32_t(i), kIdentity[i]);
    native::Log("createCloth: NaN globalPose replaced by identity");
}
