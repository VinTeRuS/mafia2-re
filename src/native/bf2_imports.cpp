// Imports that mafia2.exe needs on top of what the Spore-derived shims had: ANSI kernel32 leftovers of its
// static MSVC CRT, a few user32/gdi32 calls, Winsock (real BSD sockets), DirectSound ordinals, COM.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <cwctype>
#include <ctime>

#include "win.h"

namespace native {

uint32_t HostWindow();

namespace {

#define F(name) void f_##name(Ctx* c)

// ---------------------------------------------------------------- kernel32
F(CompareStringA) {
    uint32_t flags = Arg(c, 1), a = Arg(c, 2), b = Arg(c, 4);
    int32_t na = int32_t(Arg(c, 3)), nb = int32_t(Arg(c, 5));
    if (na < 0) na = int32_t(strlen(Str(a)));
    if (nb < 0) nb = int32_t(strlen(Str(b)));
    int32_t n = na < nb ? na : nb;
    int r = 0;
    for (int32_t i = 0; i < n && !r; ++i) {
        int x = uint8_t(Str(a)[i]), y = uint8_t(Str(b)[i]);
        if (flags & 1) x = tolower(x), y = tolower(y);  // NORM_IGNORECASE
        r = x - y;
    }
    if (!r) r = na - nb;
    Return(c, r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}
uint32_t PutString(uint32_t buf, uint32_t cap, const char* s) {
    uint32_t n = uint32_t(strlen(s));
    if (buf && cap > n) memcpy(Str(buf), s, n + 1);
    return n;
}
F(GetWindowsDirectoryA) { Return(c, PutString(Arg(c, 0), Arg(c, 1), "C:\\Windows"), 2); }
F(GetSystemDirectoryA) { Return(c, PutString(Arg(c, 0), Arg(c, 1), "C:\\Windows\\System32"), 2); }
F(IsProcessorFeaturePresent) { Return(c, 0, 1); }
F(IsBadCodePtr) { Return(c, Arg(c, 0) ? 0 : 1, 1); }
F(IsBadReadPtr) { Return(c, Arg(c, 0) ? 0 : 1, 2); }
F(IsBadWritePtr) { Return(c, Arg(c, 0) ? 0 : 1, 2); }
F(GetStringTypeA) {
    uint32_t src = Arg(c, 2), out = Arg(c, 4);
    int32_t n = int32_t(Arg(c, 3));
    if (n < 0) n = int32_t(strlen(Str(src))) + 1;
    for (int32_t i = 0; i < n; ++i) {
        int ch = uint8_t(Str(src)[i]);
        uint32_t t = 0;
        if (isupper(ch)) t |= 0x1 | 0x100;
        if (islower(ch)) t |= 0x2 | 0x100;
        if (isdigit(ch)) t |= 0x4;
        if (isspace(ch)) t |= 0x8;
        if (ispunct(ch)) t |= 0x10;
        if (iscntrl(ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80;
        W16(out + 2 * uint32_t(i), uint16_t(t));
    }
    Return(c, 1, 5);
}
F(GetEnvironmentStrings) {
    static uint32_t block = 0;
    if (!block) {
        static const char kEnv[] = "SystemRoot=C:\\Windows\0windir=C:\\Windows\0TEMP=C:\\Temp\0TMP=C:\\Temp\0";
        block = HeapAlloc(sizeof(kEnv) + 2);
        memcpy(Str(block), kEnv, sizeof(kEnv) - 1);
        Str(block)[sizeof(kEnv) - 1] = 0;
        Str(block)[sizeof(kEnv)] = 0;
    }
    Return(c, block, 0);
}
F(FreeEnvironmentStringsA) { Return(c, 1, 1); }
F(HeapCreate) { Return(c, 0x00ee0000u, 3); }
F(HeapDestroy) { Return(c, 1, 1); }
F(HeapSize) { Return(c, Arg(c, 2) ? HeapSize(Arg(c, 2)) : 0xffffffffu, 3); }
F(HeapReAlloc) {
    uint32_t old = Arg(c, 2), size = Arg(c, 3);
    uint32_t oldsize = HeapSize(old);
    uint32_t n = HeapRealloc(old, size);
    if (n && (Arg(c, 1) & 8) && size > oldsize) memset(Str(n) + oldsize, 0, size - oldsize);
    Return(c, n, 4);
}
F(SetStdHandle) { Return(c, 1, 2); }
F(SetHandleCount) { Return(c, Arg(c, 0), 1); }
F(LCMapStringA) {
    uint32_t flags = Arg(c, 1), src = Arg(c, 2), dst = Arg(c, 4), cap = Arg(c, 5);
    int32_t n = int32_t(Arg(c, 3));
    if (n < 0) n = int32_t(strlen(Str(src))) + 1;
    if (cap == 0) {
        Return(c, uint32_t(n), 6);
        return;
    }
    int32_t m = n < int32_t(cap) ? n : int32_t(cap);
    for (int32_t i = 0; i < m; ++i) {
        int ch = uint8_t(Str(src)[i]);
        if (flags & 0x100) ch = tolower(ch);
        if (flags & 0x200) ch = toupper(ch);
        Str(dst)[i] = char(ch);
    }
    Return(c, uint32_t(m), 6);
}
F(VirtualProtect) {
    if (Arg(c, 3)) W32(Arg(c, 3), 4);
    Return(c, 1, 4);
}
F(PeekNamedPipe) { Return(c, 0, 6); }
F(RtlUnwind) {
    Log("RtlUnwind called (SEH unwinding is not supported)");
    Return(c, 0, 4);
}
F(GetCPInfo) {  // single-byte code pages only (the CRT asks for 1252/437 while setting up its ctype tables)
    uint32_t p = Arg(c, 1);
    W32(p, 1);
    Str(p + 4)[0] = '?';
    Str(p + 4)[1] = 0;
    for (uint32_t i = 0; i < 12; ++i) Str(p + 6)[i] = 0;
    Return(c, 1, 2);
}
F(GetOEMCP) { Return(c, 437, 0); }
F(ExitProcess) {
    Log("ExitProcess(%u)", Arg(c, 0));
    fflush(nullptr);
    _exit(int(Arg(c, 0)));
}
F(GetDiskFreeSpaceA) {
    if (Arg(c, 1)) W32(Arg(c, 1), 64);
    if (Arg(c, 2)) W32(Arg(c, 2), 512);
    if (Arg(c, 3)) W32(Arg(c, 3), 1u << 20);
    if (Arg(c, 4)) W32(Arg(c, 4), 1u << 21);
    Return(c, 1, 5);
}
F(GetUserDefaultLangID) { Return(c, 0x0409, 0); }
F(lstrcpyA) {
    strcpy(Str(Arg(c, 0)), Str(Arg(c, 1)));
    Return(c, Arg(c, 0), 2);
}
void FileTimeToTm(uint64_t ft, struct tm* tm, uint32_t* ms) {
    uint64_t secs = ft / 10000000ull;
    *ms = uint32_t((ft / 10000ull) % 1000);
    time_t t = time_t(int64_t(secs) - 11644473600ll);
    gmtime_r(&t, tm);
}
F(FileTimeToSystemTime) {
    uint32_t in = Arg(c, 0), out = Arg(c, 1);
    uint64_t ft = uint64_t(R32(in)) | (uint64_t(R32(in + 4)) << 32);
    struct tm tm;
    uint32_t ms;
    FileTimeToTm(ft, &tm, &ms);
    W16(out, uint16_t(tm.tm_year + 1900));
    W16(out + 2, uint16_t(tm.tm_mon + 1));
    W16(out + 4, uint16_t(tm.tm_wday));
    W16(out + 6, uint16_t(tm.tm_mday));
    W16(out + 8, uint16_t(tm.tm_hour));
    W16(out + 10, uint16_t(tm.tm_min));
    W16(out + 12, uint16_t(tm.tm_sec));
    W16(out + 14, uint16_t(ms));
    Return(c, 1, 2);
}
F(FileTimeToLocalFileTime) {  // the guest's time zone is UTC
    W32(Arg(c, 1), R32(Arg(c, 0)));
    W32(Arg(c, 1) + 4, R32(Arg(c, 0) + 4));
    Return(c, 1, 2);
}
// GetDateFormatA / GetTimeFormatA(locale, flags, SYSTEMTIME*, format, buf, cch): fixed US formats.
uint32_t FormatTo(Ctx* c, const char* text) {
    uint32_t buf = Arg(c, 4), cap = Arg(c, 5);
    uint32_t n = uint32_t(strlen(text)) + 1;
    if (cap == 0) return n;
    if (cap < n) {
        SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
        return 0;
    }
    memcpy(Str(buf), text, n);
    return n;
}
F(GetDateFormatA) {
    char t[32];
    uint32_t st = Arg(c, 2);
    if (st) snprintf(t, sizeof(t), "%u/%u/%u", R16(st + 2), R16(st + 6), R16(st));
    else strcpy(t, "1/1/2005");
    Return(c, FormatTo(c, t), 6);
}
F(GetTimeFormatA) {
    char t[32];
    uint32_t st = Arg(c, 2);
    if (st) {
        unsigned h = R16(st + 8);
        snprintf(t, sizeof(t), "%u:%02u:%02u %s", h % 12 ? h % 12 : 12, R16(st + 10), R16(st + 12), h < 12 ? "AM" : "PM");
    } else {
        strcpy(t, "12:00:00 AM");
    }
    Return(c, FormatTo(c, t), 6);
}

// Fiber-local storage on thread-local slots (the UCRT of hook.dll asks for FlsAlloc through GetProcAddress).
thread_local uint32_t t_fls[128];
uint32_t g_fls_next = 1;
F(FlsAlloc) { Return(c, __atomic_fetch_add(&g_fls_next, 1, __ATOMIC_RELAXED), 1); }
F(FlsFree) { Return(c, 1, 1); }
F(FlsGetValue) { Return(c, Arg(c, 0) < 128 ? t_fls[Arg(c, 0)] : 0, 1); }
F(FlsSetValue) {
    if (Arg(c, 0) < 128) t_fls[Arg(c, 0)] = Arg(c, 1);
    Return(c, 1, 2);
}

// hook.dll's own needs (Detours patching, path checks, the UCRT startup)
F(VerifyVersionInfoW) { Return(c, 1, 4); }  // (info, type mask, 64-bit condition mask)
F(VerSetConditionMask) {  // (ULONGLONG mask, DWORD type, BYTE cond) -> ULONGLONG in edx:eax
    uint64_t mask = uint64_t(Arg(c, 0)) | (uint64_t(Arg(c, 1)) << 32);
    uint32_t type = Arg(c, 2), cond = Arg(c, 3) & 7;
    int bit = type == 1 ? 0 : type == 2 ? 1 : type == 4 ? 2 : type == 8 ? 3 : type == 16 ? 4 : type == 32 ? 5 : type == 128 ? 7 : 6;
    mask = (mask & ~(7ull << (3 * bit))) | (uint64_t(cond) << (3 * bit));
    c->edx = uint32_t(mask >> 32);
    Return(c, uint32_t(mask), 4);
}
F(WriteProcessMemory) {
    memcpy(Ptr<void>(Arg(c, 1)), Ptr<void>(Arg(c, 2)), Arg(c, 3));
    if (Arg(c, 4)) W32(Arg(c, 4), Arg(c, 3));
    Return(c, 1, 5);
}
F(UnmapViewOfFile) { Return(c, 1, 1); }
F(MapViewOfFile) { Return(c, 0, 5); }
F(CreateFileMappingW) { Return(c, 0, 6); }
F(QueryDosDeviceW) { Return(c, 0, 3); }
F(GetMappedFileNameW) { Return(c, 0, 4); }
F(GetLogicalDriveStringsW) {
    if (Arg(c, 0) >= 5 && Arg(c, 1)) {
        const uint16_t s[] = {'C', ':', '\\', 0, 0};
        for (int i = 0; i < 5; ++i) W16(Arg(c, 1) + 2u * uint32_t(i), s[i]);
    }
    Return(c, 4, 2);
}
F(EncodePointer) { Return(c, Arg(c, 0), 1); }
F(DecodePointer) { Return(c, Arg(c, 0), 1); }
F(GetStringTypeW) {  // (type, src, cch, out)
    uint32_t src = Arg(c, 1), out = Arg(c, 3);
    int32_t n = int32_t(Arg(c, 2));
    if (n < 0) {
        n = 0;
        while (R16(src + 2 * uint32_t(n))) ++n;
        ++n;
    }
    for (int32_t i = 0; i < n; ++i) {
        wint_t ch = R16(src + 2 * uint32_t(i));
        uint32_t t = 0;
        if (iswupper(ch)) t |= 0x1 | 0x100;
        if (iswlower(ch)) t |= 0x2 | 0x100;
        if (iswdigit(ch)) t |= 0x4;
        if (iswspace(ch)) t |= 0x8;
        if (iswpunct(ch)) t |= 0x10;
        if (iswcntrl(ch)) t |= 0x20;
        if (ch == ' ' || ch == '\t') t |= 0x40;
        if (iswxdigit(ch)) t |= 0x80;
        if (iswalpha(ch)) t |= 0x100;
        W16(out + 2 * uint32_t(i), uint16_t(t));
    }
    Return(c, 1, 4);
}
F(FlushInstructionCache) { Return(c, 1, 3); }
F(SetThreadContext) { Return(c, 1, 2); }
F(GetThreadContext) { Return(c, 1, 2); }
F(GetCurrentDirectoryW) {
    static const char kDir[] = "C:\\Game";
    uint32_t n = uint32_t(sizeof(kDir) - 1);
    if (Arg(c, 0) > n) {
        for (uint32_t i = 0; i <= n; ++i) W16(Arg(c, 1) + 2 * i, uint16_t(uint8_t(kDir[i])));
        Return(c, n, 2);
    } else {
        Return(c, n + 1, 2);
    }
}
F(IsValidCodePage) { Return(c, 1, 1); }
F(GetStartupInfoW) {
    for (uint32_t o = 0; o < 68; o += 4) W32(Arg(c, 0) + o, 0);
    W32(Arg(c, 0), 68);
    Return(c, 0, 1);
}
F(GetEnvironmentStringsW) {
    static uint32_t block = 0;
    if (!block) {
        block = HeapAlloc(16);
        for (int i = 0; i < 8; ++i) W16(block + 2u * uint32_t(i), 0);
    }
    Return(c, block, 0);
}
F(FreeEnvironmentStringsW) { Return(c, 1, 1); }
F(GetUserDefaultLCID) { Return(c, 0x0409, 0); }
F(EnumSystemLocalesA) { Return(c, 1, 2); }
F(IsValidLocale) { Return(c, 1, 2); }
F(GetDriveTypeW) { Return(c, 3, 1); }
F(GetConsoleCP) { Return(c, 437, 0); }
F(GetConsoleMode) { Return(c, 0, 2); }
F(WriteConsoleW) { Return(c, 0, 5); }

// ---------------------------------------------------------------- user32 / gdi32
F(FindWindowA) { Return(c, 0, 2); }
F(GetDlgItem) { Return(c, 0, 2); }
F(EndDialog) { Return(c, 1, 2); }
F(SetWindowTextA) { Return(c, 1, 2); }
F(DialogBoxParamA) { Return(c, 0, 5); }
F(GetFocus) { Return(c, HostWindow(), 0); }
F(DestroyMenu) { Return(c, 1, 1); }
F(DrawTextA) { Return(c, 0, 5); }
F(SetRect) {
    uint32_t r = Arg(c, 0);
    for (int i = 0; i < 4; ++i) W32(r + 4 * uint32_t(i), Arg(c, 1 + i));
    Return(c, 1, 5);
}
F(AdjustWindowRect) { Return(c, 1, 3); }  // borderless: the client area is the window
F(GetWindowPlacement) {
    uint32_t p = Arg(c, 1);
    for (uint32_t o = 0; o < 44; o += 4) W32(p + o, 0);
    W32(p, 44);
    W32(p + 8, 1);  // SW_SHOWNORMAL
    Return(c, 1, 2);
}
F(MapVirtualKeyA) { Return(c, MapVirtualKeyHost(Arg(c, 0), Arg(c, 1)), 2); }

// US layout: virtual key + shift state -> character (0: none).
int VkToChar(uint32_t vk, bool shift, bool caps) {
    if (vk >= 'A' && vk <= 'Z') return (shift != caps) ? int(vk) : int(vk) + 32;
    if (vk >= '0' && vk <= '9') return shift ? ")!@#$%^&*("[vk - '0'] : int(vk);
    if (vk >= 0x60 && vk <= 0x69) return int('0' + (vk - 0x60));
    switch (vk) {
        case 0x20: return ' ';
        case 0xba: return shift ? ':' : ';';
        case 0xbb: return shift ? '+' : '=';
        case 0xbc: return shift ? '<' : ',';
        case 0xbd: return shift ? '_' : '-';
        case 0xbe: return shift ? '>' : '.';
        case 0xbf: return shift ? '?' : '/';
        case 0xc0: return shift ? '~' : '`';
        case 0xdb: return shift ? '{' : '[';
        case 0xdc: return shift ? '|' : '\\';
        case 0xdd: return shift ? '}' : ']';
        case 0xde: return shift ? '"' : '\'';
        case 0x6a: return '*';
        case 0x6b: return '+';
        case 0x6d: return '-';
        case 0x6e: return '.';
        case 0x6f: return '/';
        default: return 0;
    }
}
int KeyChar(Ctx* c) {
    uint32_t state = Arg(c, 2);
    bool shift = state && (Str(state)[0x10] & 0x80);
    bool caps = state && (Str(state)[0x14] & 1);
    return VkToChar(Arg(c, 0), shift, caps);
}
F(ToAscii) {
    int ch = KeyChar(c);
    if (ch) W16(Arg(c, 3), uint16_t(ch));
    Return(c, ch ? 1 : 0, 5);
}
F(ToUnicode) {
    int ch = KeyChar(c);
    if (ch && Arg(c, 4) >= 1) W16(Arg(c, 3), uint16_t(ch));
    Return(c, ch ? 1 : 0, 6);
}

F(CreateCompatibleDC) { Return(c, 0x5001, 1); }
F(SetMapMode) { Return(c, 1, 2); }
F(GetObjectA) { Return(c, 0, 3); }
F(TextOutA) { Return(c, 1, 5); }
F(LineTo) { Return(c, 1, 3); }
F(SetTextColor) { Return(c, 0, 2); }
F(SetBkColor) { Return(c, 0, 2); }
F(SetBkMode) { Return(c, 1, 2); }
F(CreateDIBSection) {
    uint32_t bmi = Arg(c, 1), pbits = Arg(c, 3);
    int32_t w = int32_t(R32(bmi + 4)), h = int32_t(R32(bmi + 8));
    uint32_t bpp = R16(bmi + 14);
    if (h < 0) h = -h;
    uint32_t size = ((uint32_t(w) * bpp + 31) / 32 * 4) * uint32_t(h);
    uint32_t bits = size ? HeapAlloc(size) : 0;
    if (bits) memset(Str(bits), 0, size);
    if (pbits) W32(pbits, bits);
    Return(c, bits ? 0x5005 : 0, 6);
}
F(CreateFontIndirectA) { Return(c, 0x5002, 1); }
F(GdiFlush) { Return(c, 1, 0); }
F(GetDeviceGammaRamp) { Return(c, 0, 2); }
F(SetICMMode) { Return(c, 1, 2); }
F(SelectObject) { Return(c, 0x5003, 2); }
F(GetStockObject) { Return(c, 0x5004, 1); }
F(MoveToEx) { Return(c, 1, 4); }

// ---------------------------------------------------------------- ole32 / winmm
F(CoInitialize) { Return(c, 0, 1); }
F(CoUninitialize) { Return(c, 0, 0); }
F(CoCreateInstance) {
    if (Arg(c, 4)) W32(Arg(c, 4), 0);
    Return(c, 0x80040154u, 5);  // REGDB_E_CLASSNOTREG
}
F(timeGetTime) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static const timespec t0 = [] { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t; }();
    double ms = double(ts.tv_sec - t0.tv_sec) * 1000.0 + double(ts.tv_nsec - t0.tv_nsec) / 1e6;
    static const double scale = SettingInt("time_scale", 1) > 1 ? double(SettingInt("time_scale", 1)) : 1.0;
    Return(c, uint32_t(ms * scale), 0);
}

// ---------------------------------------------------------------- DirectSound ordinals
// DirectSoundEnumerateA: one default device (NULL GUID).
F(DirectSoundEnumerateA) {
    uint32_t cb = Arg(c, 0), ctx = Arg(c, 1);
    static uint32_t desc = GuestStrdup("Primary Sound Driver"), mod = GuestStrdup("");
    if (cb) GuestCall(cb, {0, desc, mod, ctx});
    Return(c, 0, 2);
}
F(DirectSoundCaptureEnumerateA) { Return(c, 0, 2); }
F(DirectSoundCaptureCreate8) {
    if (Arg(c, 1)) W32(Arg(c, 1), 0);
    Return(c, 0x88780078u, 3);  // DSERR_NODRIVER
}

// ---------------------------------------------------------------- Winsock on BSD sockets
thread_local uint32_t t_wsa_error = 0;

uint32_t WsaError(int e) {
    switch (e) {
        case EWOULDBLOCK: return 10035;
        case EINPROGRESS: return 10036;
        case EALREADY: return 10037;
        case ENOTSOCK: return 10038;
        case EMSGSIZE: return 10040;
        case EPROTONOSUPPORT: return 10043;
        case EOPNOTSUPP: return 10045;
        case EAFNOSUPPORT: return 10047;
        case EADDRINUSE: return 10048;
        case EADDRNOTAVAIL: return 10049;
        case ENETUNREACH: return 10051;
        case ECONNABORTED: return 10053;
        case ECONNRESET: return 10054;
        case ENOTCONN: return 10057;
        case ETIMEDOUT: return 10060;
        case ECONNREFUSED: return 10061;
        case EHOSTUNREACH: return 10065;
        case EINVAL: return 10022;
        case EBADF: return 10009;
        case EACCES: return 10013;
        case EFAULT: return 10014;
        case EINTR: return 10004;
        default: return 10000u + uint32_t(e);
    }
}
uint32_t SockResult(Ctx* c, long r, int args) {
    if (r < 0) {
        t_wsa_error = WsaError(errno);
        Return(c, 0xffffffffu, args);
        return 0;
    }
    Return(c, uint32_t(r), args);
    return 1;
}

F(WSAStartup) {
    uint32_t d = Arg(c, 1);
    if (d) {
        for (uint32_t o = 0; o < 400; o += 4) W32(d + o, 0);
        W16(d, uint16_t(Arg(c, 0)));
        W16(d + 2, 0x0202);
        strcpy(Str(d + 4), "WinSock 2.0");
        W16(d + 390, 32767);
        W16(d + 392, 65467);
    }
    Return(c, 0, 2);
}
F(WSACleanup) { Return(c, 0, 0); }
F(WSAGetLastError) { Return(c, t_wsa_error, 0); }
F(socket) {
    int type = int(Arg(c, 1)), proto = int(Arg(c, 2));
    SockResult(c, ::socket(int(Arg(c, 0)), type, proto), 3);
}
F(closesocket) { SockResult(c, ::close(int(Arg(c, 0))), 1); }
F(bind) { SockResult(c, ::bind(int(Arg(c, 0)), Ptr<sockaddr>(Arg(c, 1)), socklen_t(Arg(c, 2))), 3); }
F(connect) {
    long r = ::connect(int(Arg(c, 0)), Ptr<sockaddr>(Arg(c, 1)), socklen_t(Arg(c, 2)));
    if (r < 0 && errno == EINPROGRESS) errno = EWOULDBLOCK;
    SockResult(c, r, 3);
}
F(listen) { SockResult(c, ::listen(int(Arg(c, 0)), int(Arg(c, 1))), 2); }
F(accept) {
    socklen_t len = Arg(c, 2) ? socklen_t(R32(Arg(c, 2))) : 0;
    long r = ::accept(int(Arg(c, 0)), Ptr<sockaddr>(Arg(c, 1)), Arg(c, 2) ? &len : nullptr);
    if (r >= 0 && Arg(c, 2)) W32(Arg(c, 2), len);
    SockResult(c, r, 3);
}
F(send) { SockResult(c, ::send(int(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2), MSG_NOSIGNAL), 4); }
F(recv) { SockResult(c, ::recv(int(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2), int(Arg(c, 3)) & 2), 4); }
F(sendto) {
    SockResult(c, ::sendto(int(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2), MSG_NOSIGNAL, Ptr<sockaddr>(Arg(c, 4)),
                           socklen_t(Arg(c, 5))), 6);
}
F(recvfrom) {
    socklen_t len = Arg(c, 5) ? socklen_t(R32(Arg(c, 5))) : 0;
    long r = ::recvfrom(int(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2), int(Arg(c, 3)) & 2, Ptr<sockaddr>(Arg(c, 4)),
                        Arg(c, 5) ? &len : nullptr);
    if (r >= 0 && Arg(c, 5)) W32(Arg(c, 5), len);
    SockResult(c, r, 6);
}
F(ioctlsocket) {
    int fd = int(Arg(c, 0));
    uint32_t cmd = Arg(c, 1), arg = Arg(c, 2);
    if (cmd == 0x8004667eu) {  // FIONBIO
        int fl = fcntl(fd, F_GETFL, 0);
        fl = R32(arg) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
        SockResult(c, fcntl(fd, F_SETFL, fl), 3);
    } else if (cmd == 0x4004667fu) {  // FIONREAD
        int n = 0;
        long r = ioctl(fd, FIONREAD, &n);
        W32(arg, uint32_t(n));
        SockResult(c, r, 3);
    } else {
        t_wsa_error = 10022;
        Return(c, 0xffffffffu, 3);
    }
}
bool MapSockOpt(uint32_t level, uint32_t opt, int* hl, int* ho) {
    if (level == 0xffff) {
        *hl = SOL_SOCKET;
        switch (opt) {
            case 0x4: *ho = SO_REUSEADDR; return true;
            case 0x8: *ho = SO_KEEPALIVE; return true;
            case 0x20: *ho = SO_BROADCAST; return true;
            case 0x1001: *ho = SO_SNDBUF; return true;
            case 0x1002: *ho = SO_RCVBUF; return true;
            case 0x1007: *ho = SO_ERROR; return true;
            case 0x1008: *ho = SO_TYPE; return true;
            default: return false;
        }
    }
    if (level == 6 && opt == 1) {
        *hl = IPPROTO_TCP;
        *ho = TCP_NODELAY;
        return true;
    }
    return false;
}
F(setsockopt) {
    int hl, ho;
    if (!MapSockOpt(Arg(c, 1), Arg(c, 2), &hl, &ho)) {
        Return(c, 0, 5);  // options with no host equivalent (SO_DONTLINGER, ...) are accepted
        return;
    }
    int v = 0;
    memcpy(&v, Ptr<void>(Arg(c, 3)), Arg(c, 4) < 4 ? Arg(c, 4) : 4);
    SockResult(c, ::setsockopt(int(Arg(c, 0)), hl, ho, &v, sizeof(v)), 5);
}
F(getsockopt) {
    int hl, ho;
    if (!MapSockOpt(Arg(c, 1), Arg(c, 2), &hl, &ho)) {
        t_wsa_error = 10042;  // WSAENOPROTOOPT
        Return(c, 0xffffffffu, 5);
        return;
    }
    int v = 0;
    socklen_t len = sizeof(v);
    long r = ::getsockopt(int(Arg(c, 0)), hl, ho, &v, &len);
    if (r == 0) {
        W32(Arg(c, 3), uint32_t(v));
        W32(Arg(c, 4), 4);
    }
    SockResult(c, r, 5);
}
F(getsockname) {
    socklen_t len = socklen_t(R32(Arg(c, 2)));
    long r = ::getsockname(int(Arg(c, 0)), Ptr<sockaddr>(Arg(c, 1)), &len);
    if (r == 0) W32(Arg(c, 2), len);
    SockResult(c, r, 3);
}
F(shutdown) { SockResult(c, ::shutdown(int(Arg(c, 0)), int(Arg(c, 1))), 2); }
// fd_set of Winsock: u_int count; SOCKET array[64].
F(select) {
    struct Set {
        uint32_t addr;
        short events;
    } sets[3] = {{Arg(c, 1), POLLIN}, {Arg(c, 2), POLLOUT}, {Arg(c, 3), POLLPRI}};
    std::vector<pollfd> fds;
    for (auto& s : sets) {
        if (!s.addr) continue;
        uint32_t n = R32(s.addr);
        for (uint32_t i = 0; i < n && i < 64; ++i) fds.push_back({int(R32(s.addr + 4 + 4 * i)), s.events, 0});
    }
    int timeout = -1;
    if (Arg(c, 4)) timeout = int(int32_t(R32(Arg(c, 4))) * 1000 + int32_t(R32(Arg(c, 4) + 4)) / 1000);
    int r = 0;
    if (fds.empty()) {
        if (timeout > 0) usleep(useconds_t(timeout) * 1000);
    } else {
        r = ::poll(fds.data(), fds.size(), timeout);
    }
    if (r < 0) {
        SockResult(c, -1, 5);
        return;
    }
    size_t k = 0;
    uint32_t total = 0;
    for (auto& s : sets) {
        if (!s.addr) continue;
        uint32_t n = R32(s.addr), out = 0;
        for (uint32_t i = 0; i < n && i < 64; ++i, ++k) {
            short re = fds[k].revents;
            bool hit = s.events == POLLIN ? (re & (POLLIN | POLLHUP | POLLERR)) : s.events == POLLOUT ? (re & (POLLOUT | POLLERR)) : (re & POLLPRI);
            if (hit) W32(s.addr + 4 + 4 * out++, uint32_t(fds[k].fd));
        }
        W32(s.addr, out);
        total += out;
    }
    Return(c, total, 5);
}
F(__WSAFDIsSet) {
    uint32_t s = Arg(c, 0), set = Arg(c, 1), n = R32(set);
    uint32_t r = 0;
    for (uint32_t i = 0; i < n && i < 64; ++i)
        if (R32(set + 4 + 4 * i) == s) r = 1;
    Return(c, r, 2);
}
F(gethostname) {
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0) strcpy(buf, "localhost");
    PutString(Arg(c, 0), Arg(c, 1), buf);
    Return(c, 0, 2);
}
F(gethostbyname) {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(Str(Arg(c, 0)), nullptr, &hints, &res) != 0 || !res) {
        t_wsa_error = 11001;  // WSAHOST_NOT_FOUND
        Return(c, 0, 1);
        return;
    }
    static thread_local uint32_t block = 0;
    if (!block) block = HeapAlloc(128);
    // hostent: name, aliases, addrtype(2), length(2), addr_list; then the list, the address, the name.
    uint32_t b = block;
    W32(b, b + 32);
    W32(b + 4, b + 24);
    W16(b + 8, 2);
    W16(b + 10, 4);
    W32(b + 12, b + 16);
    W32(b + 16, b + 28);
    W32(b + 20, 0);
    W32(b + 24, 0);
    W32(b + 28, reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
    strncpy(Str(b + 32), Str(Arg(c, 0)), 90);
    Str(b + 32)[90] = 0;
    freeaddrinfo(res);
    Return(c, b, 1);
}
F(inet_addr) {
    in_addr_t a = ::inet_addr(Str(Arg(c, 0)));
    Return(c, a, 1);
}
F(inet_ntoa) {
    static thread_local uint32_t buf = 0;
    if (!buf) buf = HeapAlloc(32);
    in_addr a;
    a.s_addr = Arg(c, 0);
    strcpy(Str(buf), ::inet_ntoa(a));
    Return(c, buf, 1);
}
F(htons) { Return(c, uint32_t(__builtin_bswap16(uint16_t(Arg(c, 0)))), 1); }
F(ntohs) { Return(c, uint32_t(__builtin_bswap16(uint16_t(Arg(c, 0)))), 1); }
F(htonl) { Return(c, __builtin_bswap32(Arg(c, 0)), 1); }
F(ntohl) { Return(c, __builtin_bswap32(Arg(c, 0)), 1); }

#undef F

#define K(n) {"kernel32.dll", #n, f_##n}
#define U(n) {"user32.dll", #n, f_##n}
#define G(n) {"gdi32.dll", #n, f_##n}
#define W(n) {"ws2_32.dll", #n, f_##n}, {"wsock32.dll", #n, f_##n}
const ImportDef kBf2[] = {
    K(CompareStringA), K(GetWindowsDirectoryA), K(GetSystemDirectoryA), K(IsProcessorFeaturePresent), K(IsBadCodePtr),
    K(IsBadReadPtr), K(IsBadWritePtr), K(GetStringTypeA), K(GetEnvironmentStrings), K(FreeEnvironmentStringsA),
    K(HeapCreate), K(HeapDestroy), K(HeapSize), K(HeapReAlloc), K(SetStdHandle), K(SetHandleCount), K(LCMapStringA),
    K(VirtualProtect), K(PeekNamedPipe), K(RtlUnwind), K(GetOEMCP), K(GetCPInfo), K(ExitProcess), K(GetDiskFreeSpaceA),
    K(GetUserDefaultLangID), K(lstrcpyA), K(FileTimeToSystemTime), K(FileTimeToLocalFileTime), K(GetDateFormatA),
    K(GetTimeFormatA), K(VerifyVersionInfoW), K(VerSetConditionMask), K(WriteProcessMemory), K(UnmapViewOfFile),
    K(MapViewOfFile), K(CreateFileMappingW), K(QueryDosDeviceW), K(GetLogicalDriveStringsW), K(EncodePointer),
    K(DecodePointer), K(GetStringTypeW), K(FlushInstructionCache), K(SetThreadContext), K(GetThreadContext),
    K(GetCurrentDirectoryW), K(IsValidCodePage), K(GetStartupInfoW), K(GetEnvironmentStringsW),
    K(FreeEnvironmentStringsW), K(GetUserDefaultLCID), K(EnumSystemLocalesA), K(IsValidLocale), K(GetDriveTypeW),
    K(FlsAlloc), K(FlsFree), K(FlsGetValue), K(FlsSetValue), K(GetConsoleCP), K(GetConsoleMode), K(WriteConsoleW),
    {"psapi.dll", "GetMappedFileNameW", f_GetMappedFileNameW},
    U(FindWindowA), U(GetDlgItem), U(EndDialog), U(SetWindowTextA), U(DialogBoxParamA), U(GetFocus), U(DestroyMenu),
    U(DrawTextA), U(SetRect), U(AdjustWindowRect), U(GetWindowPlacement), U(MapVirtualKeyA), U(ToAscii), U(ToUnicode),
    G(CreateCompatibleDC), G(SetMapMode), G(GetObjectA), G(TextOutA), G(LineTo), G(SetTextColor), G(SetBkColor),
    G(SetBkMode), G(CreateDIBSection), G(CreateFontIndirectA), G(GdiFlush), G(GetDeviceGammaRamp), G(SetICMMode),
    G(SelectObject), G(GetStockObject), G(MoveToEx),
    {"ole32.dll", "CoInitialize", f_CoInitialize}, {"ole32.dll", "CoUninitialize", f_CoUninitialize},
    {"ole32.dll", "CoCreateInstance", f_CoCreateInstance}, {"winmm.dll", "timeGetTime", f_timeGetTime},
    {"dsound.dll", "#2", f_DirectSoundEnumerateA}, {"dsound.dll", "#7", f_DirectSoundCaptureEnumerateA},
    {"dsound.dll", "#12", f_DirectSoundCaptureCreate8},
    W(WSAStartup), W(WSACleanup), W(WSAGetLastError), W(socket), W(closesocket), W(bind), W(connect), W(listen),
    W(accept), W(send), W(recv), W(sendto), W(recvfrom), W(ioctlsocket), W(setsockopt), W(getsockopt),
    W(getsockname), W(shutdown), W(select), W(__WSAFDIsSet), W(gethostname), W(gethostbyname), W(inet_addr),
    W(inet_ntoa), W(htons), W(ntohs), W(htonl), W(ntohl),
    // ws2_32 imports by ordinal
    {"ws2_32.dll", "#1", f_accept},
    {"ws2_32.dll", "#2", f_bind},
    {"ws2_32.dll", "#3", f_closesocket},
    {"ws2_32.dll", "#4", f_connect},
    {"ws2_32.dll", "#6", f_getsockname},
    {"ws2_32.dll", "#7", f_getsockopt},
    {"ws2_32.dll", "#8", f_htonl},
    {"ws2_32.dll", "#9", f_htons},
    {"ws2_32.dll", "#10", f_ioctlsocket},
    {"ws2_32.dll", "#11", f_inet_addr},
    {"ws2_32.dll", "#12", f_inet_ntoa},
    {"ws2_32.dll", "#13", f_listen},
    {"ws2_32.dll", "#14", f_ntohl},
    {"ws2_32.dll", "#15", f_ntohs},
    {"ws2_32.dll", "#16", f_recv},
    {"ws2_32.dll", "#17", f_recvfrom},
    {"ws2_32.dll", "#18", f_select},
    {"ws2_32.dll", "#19", f_send},
    {"ws2_32.dll", "#20", f_sendto},
    {"ws2_32.dll", "#21", f_setsockopt},
    {"ws2_32.dll", "#22", f_shutdown},
    {"ws2_32.dll", "#23", f_socket},
    {"ws2_32.dll", "#52", f_gethostbyname},
    {"ws2_32.dll", "#57", f_gethostname},
    {"ws2_32.dll", "#111", f_WSAGetLastError},
    {"ws2_32.dll", "#115", f_WSAStartup},
    {"ws2_32.dll", "#116", f_WSACleanup},
    {"ws2_32.dll", "#151", f___WSAFDIsSet},
};
ImportRegistrar g_bf2(kBf2);

}  // namespace
}  // namespace native
