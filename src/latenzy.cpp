// latenzy.cpp — Latenzy: lightweight HTTP latency monitor (Win32 + GDI+)
// build: g++ -std=c++20 -O2 -municode -DUNICODE -D_UNICODE latenzy.cpp
//        -o latenzy.exe -lwinhttp -liphlpapi -lshell32 -luser32 -lgdi32 -lcomctl32
//        -lgdiplus -luxtheme -lole32 -mwindows -static

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <richedit.h>
#include <uxtheme.h>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdarg>
#include <cwchar>

using namespace Gdiplus;

#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")

// ─── control IDs ─────────────────────────────────────────────
#define ID_EDIT_TARGET     2001
#define ID_EDIT_INTERVAL   2002
#define ID_BTN_START       2003
#define ID_BTN_RELOAD      2004
#define ID_BTN_CLEAR       2005
#define ID_BTN_EXIT        2006
#define ID_LOG_EDIT        2009

// ─── tray / messages / timers ────────────────────────────────
#define WM_TRAYICON     (WM_USER + 1)
#define ID_TRAY_SHOW    3001
#define ID_TRAY_TOGGLE  3002
#define ID_TRAY_RELOAD  3003
#define ID_TRAY_EXIT    3004
#define TIMER_UI        1

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// ─── theme ───────────────────────────────────────────────────
namespace theme {
    constexpr COLORREF bg           = RGB(15, 17, 21);
    constexpr COLORREF surface      = RGB(23, 26, 33);
    constexpr COLORREF surface2     = RGB(30, 34, 43);
    constexpr COLORREF surfaceHover = RGB(41, 46, 58);
    constexpr COLORREF border       = RGB(42, 47, 58);
    constexpr COLORREF text         = RGB(230, 233, 239);
    constexpr COLORREF logText      = RGB(190, 198, 212);
    constexpr COLORREF muted        = RGB(138, 146, 163);
    constexpr COLORREF accent       = RGB(91, 140, 255);
    constexpr COLORREF accentHover  = RGB(120, 163, 255);
    constexpr COLORREF accentPress  = RGB(68, 112, 228);
    constexpr COLORREF success      = RGB(52, 211, 153);
    constexpr COLORREF danger       = RGB(248, 113, 113);
    constexpr COLORREF warning      = RGB(251, 191, 36);
    constexpr COLORREF logBg        = RGB(12, 14, 18);
}

// ─── config + state ──────────────────────────────────────────
struct Config {
    std::wstring target_url = L"https://www.google.com";
    int interval_seconds = 10;
};

struct LastResult {
    bool ok = false;
    DWORD latency = 0;
    DWORD code = 0;
    std::wstring error;
    bool has_result = false;
};

struct LogEntry {
    std::wstring ts;
    std::wstring msg;
    int level = 0;      // 0 info, 1 ok, 2 fail, 3 warn
};

static const size_t MAX_HIST = 60;

static Config g_cfg;
static std::mutex g_cfg_mtx;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_active{true};
static std::atomic<bool> g_kick{false};
static std::atomic<DWORD> g_sent{0};
static std::atomic<DWORD> g_failed{0};

static std::mutex g_last_mtx;
static LastResult g_last;
static std::vector<int> g_hist;          // latency ms, -1 = failure

static std::mutex g_log_mtx;
static std::vector<LogEntry> g_logs;
static size_t g_log_consumed = 0;

static Config GetCfg() {
    std::lock_guard<std::mutex> lk(g_cfg_mtx);
    return g_cfg;
}

// ─── UI handles ──────────────────────────────────────────────
static HWND g_hwnd = nullptr;
static HWND g_h_target, g_h_interval, g_h_start, g_h_reload;
static HWND g_h_clear, g_h_exit, g_h_log;
static HWND g_hover = nullptr;
static UINT g_dpi = 96;

static HFONT g_f_ui, g_f_small, g_f_semi, g_f_value, g_f_title, g_f_logo, g_f_btn;
static HBRUSH g_bg_brush = nullptr;
static HBRUSH g_edit_brush = nullptr;

static NOTIFYICONDATAW g_nid = {};
static HICON g_icons[4] = {};            // wait/ok/fail/paused
static int g_tray_state = -1;
static std::wstring g_tray_tip;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

// ─── portable formatter ─────────────────────────────────────
static std::wstring wfmt(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 1024, fmt, args);
    va_end(args);
    return std::wstring(buf);
}

// ─── ini persistence ────────────────────────────────────────
static std::wstring GetIniPath() {
    // Portable mode: if latenzy.ini sits next to the exe, keep using it.
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\");
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    std::wstring portable = p + L"latenzy.ini";
    if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) return portable;

    // Installed mode: per-user %APPDATA%\ZPinger\latenzy.ini (always writable)
    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring dir = std::wstring(appdata) + L"\\Latenzy";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\latenzy.ini";
    }
    return portable;
}

static void LoadConfig() {
    std::wifstream f(GetIniPath().c_str());
    if (!f.is_open()) return;
    std::wstring line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = line.substr(0, eq);
        std::wstring val = line.substr(eq + 1);
        if (key == L"target") g_cfg.target_url = val;
        else if (key == L"interval") g_cfg.interval_seconds = _wtoi(val.c_str());
    }
    if (g_cfg.interval_seconds < 1) g_cfg.interval_seconds = 1;
    if (g_cfg.interval_seconds > 3600) g_cfg.interval_seconds = 3600;
}

static void SaveConfig() {
    Config c = GetCfg();
    std::wofstream f(GetIniPath().c_str());
    f << L"# Latenzy configuration\n";
    f << L"target=" << c.target_url << L"\n";
    f << L"interval=" << c.interval_seconds << L"\n";
}

// ─── network interface check ────────────────────────────────
static bool HasActiveInterface() {
    ULONG size = 0;
    GetIfTable(nullptr, &size, FALSE);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    PMIB_IFTABLE table = reinterpret_cast<PMIB_IFTABLE>(buf.data());
    if (GetIfTable(table, &size, FALSE) != NO_ERROR) return false;
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        if (table->table[i].dwOperStatus == MIB_IF_OPER_STATUS_CONNECTED ||
            table->table[i].dwOperStatus == MIB_IF_OPER_STATUS_OPERATIONAL) {
            if (table->table[i].dwType == IF_TYPE_ETHERNET_CSMACD ||
                table->table[i].dwType == IF_TYPE_IEEE80211) {
                return true;
            }
        }
    }
    return false;
}

// ─── HTTP HEAD ──────────────────────────────────────────────
struct PingResult {
    bool ok = false;
    DWORD status_code = 0;
    DWORD latency_ms = 0;
    std::wstring error;
};

static PingResult SendHeadRequest(const std::wstring& url) {
    PingResult result;
    auto t0 = std::chrono::steady_clock::now();

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.length(), 0, &uc)) {
        result.error = L"invalid URL";
        return result;
    }

    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    HINTERNET session = WinHttpOpen(L"Latenzy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { result.error = L"WinHttpOpen failed"; return result; }

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        result.error = L"connect failed";
        return result;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"HEAD", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = L"request open failed";
        return result;
    }

    WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);
    BOOL sent = WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    if (!sent) {
        result.error = L"send failed";
    } else if (!WinHttpReceiveResponse(request, nullptr)) {
        result.error = L"receive failed";
    } else {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        result.status_code = code;
        result.ok = (code >= 200 && code < 400);
    }

    auto t1 = std::chrono::steady_clock::now();
    result.latency_ms = (DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

// ─── log push (thread-safe) ─────────────────────────────────
static void PushLog(const std::wstring& msg, int level = 0) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    LogEntry e;
    e.ts = wfmt(L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    e.msg = msg;
    e.level = level;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_logs.push_back(std::move(e));
    if (g_logs.size() > 20000) {
        g_logs.erase(g_logs.begin(), g_logs.begin() + 10000);
        if (g_log_consumed > 10000) g_log_consumed -= 10000;
        else g_log_consumed = 0;
    }
}

static void PushHist(int v) {
    g_hist.push_back(v);
    if (g_hist.size() > MAX_HIST) g_hist.erase(g_hist.begin());
}

// ─── worker thread ──────────────────────────────────────────
static void PingWorker() {
    while (g_running) {
        if (!g_active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        if (!HasActiveInterface()) {
            PushLog(L"no active network interface - waiting", 3);
            {
                std::lock_guard<std::mutex> lk(g_last_mtx);
                g_last.ok = false;
                g_last.error = L"no interface";
                g_last.has_result = true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        Config cfg = GetCfg();
        g_kick = false;
        PingResult r = SendHeadRequest(cfg.target_url);
        std::wstring short_url = cfg.target_url.substr(0, 60);

        g_sent.fetch_add(1);
        if (r.ok) {
            PushLog(wfmt(L"%4lu ms   %s   HTTP %lu",
                r.latency_ms, short_url.c_str(), r.status_code), 1);
        } else {
            g_failed.fetch_add(1);
            std::wstring err = r.error.empty()
                ? wfmt(L"HTTP %lu", r.status_code) : r.error;
            PushLog(wfmt(L"FAIL     %s   (%s)", short_url.c_str(), err.c_str()), 2);
        }

        {
            std::lock_guard<std::mutex> lk(g_last_mtx);
            g_last.ok = r.ok;
            g_last.latency = r.latency_ms;
            g_last.code = r.status_code;
            g_last.error = r.error;
            g_last.has_result = true;
            PushHist(r.ok ? (int)r.latency_ms : -1);
        }

        int total_ms = cfg.interval_seconds * 1000;
        int slept = 0;
        while (slept < total_ms && g_running) {
            int chunk = (std::min)(100, total_ms - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            slept += chunk;
            if (!g_active || g_kick.exchange(false)) break;
        }
    }
}

// ═══════════════════════════════════════════════════════════
//  Rendering helpers (GDI+ shapes, GDI text)
// ═══════════════════════════════════════════════════════════
struct Painter {
    HDC dc;
    Graphics g;
    explicit Painter(HDC d) : dc(d), g(d) {
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
    }
};

static Color GC(COLORREF c, BYTE a = 255) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void RoundPath(GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = (std::min)(r * 2, (std::min)(w, h));
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(Painter& P, const RECT& rc, int radius, COLORREF fill,
                      BYTE fillAlpha = 255, COLORREF border = CLR_INVALID, float bw = 1.0f) {
    float x = rc.left + 0.5f, y = rc.top + 0.5f;
    float w = (float)(rc.right - rc.left) - 1.0f, h = (float)(rc.bottom - rc.top) - 1.0f;
    GraphicsPath path;
    RoundPath(path, x, y, w, h, (float)radius);
    if (fill != CLR_INVALID) {
        SolidBrush b(GC(fill, fillAlpha));
        P.g.FillPath(&b, &path);
    }
    if (border != CLR_INVALID) {
        Pen pen(GC(border), bw);
        P.g.DrawPath(&pen, &path);
    }
}

static void Txt(Painter& P, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    P.g.Flush(FlushIntentionSync);
    HGDIOBJ old = SelectObject(P.dc, f);
    SetBkMode(P.dc, TRANSPARENT);
    SetTextColor(P.dc, c);
    DrawTextW(P.dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
    SelectObject(P.dc, old);
}

// ─── layout ─────────────────────────────────────────────────
struct Layout {
    RECT header, logo, pill;
    RECT fTarget, fInterval, eTarget, eInterval, bApply, bPause;
    RECT card[4];
    RECT graph;
    RECT logLabel, logBox, logEdit, bClear;
    RECT footerText, bExit;
};
static Layout L = {};

static RECT Rc(int l, int t, int r, int b) { RECT x = { l, t, r, b }; return x; }

static void ComputeLayout(int W, int H) {
    int pad = S(20), gap = S(10);
    int y = pad;

    L.header = Rc(pad, y, W - pad, y + S(48));
    L.logo   = Rc(pad, y + S(6), pad + S(36), y + S(42));
    L.pill   = Rc(W - pad - S(170), y + S(8), W - pad, y + S(40));
    y += S(48) + S(16);

    int fh = S(40), bw = S(96), eh = S(20);
    int right = W - pad;
    L.bPause = Rc(right - bw, y, right, y + fh);           right -= bw + gap;
    L.bApply = Rc(right - bw, y, right, y + fh);           right -= bw + gap;
    L.fInterval = Rc(right - S(150), y, right, y + fh);    right -= S(150) + gap;
    L.fTarget = Rc(pad, y, right, y + fh);

    int ey = y + (fh - eh) / 2;
    L.eTarget   = Rc(L.fTarget.left + S(52), ey, L.fTarget.right - S(12), ey + eh);
    L.eInterval = Rc(L.fInterval.left + S(56), ey, L.fInterval.left + S(56) + S(40), ey + eh);
    y += fh + S(16);

    int total = W - pad * 2;
    int cw = (total - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        int x = pad + i * (cw + gap);
        L.card[i] = Rc(x, y, (i == 3) ? W - pad : x + cw, y + S(92));
    }
    y += S(92) + S(12);

    L.graph = Rc(pad, y, W - pad, y + S(120));
    y += S(120) + S(16);

    L.logLabel = Rc(pad, y, W - pad - S(90), y + S(28));
    L.bClear   = Rc(W - pad - S(84), y, W - pad, y + S(28));
    y += S(28) + S(6);

    int footerH = S(32);
    int footerTop = H - pad - footerH;
    int logBottom = footerTop - S(12);
    if (logBottom < y + S(80)) logBottom = y + S(80);
    L.logBox  = Rc(pad, y, W - pad, logBottom);
    L.logEdit = Rc(L.logBox.left + S(12), L.logBox.top + S(8), L.logBox.right - S(6), L.logBox.bottom - S(8));
    L.footerText = Rc(pad, footerTop, W - pad - S(100), footerTop + footerH);
    L.bExit = Rc(W - pad - S(84), footerTop, W - pad, footerTop + footerH);
}

static void Place(HWND h, const RECT& r) {
    if (!h) return;
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ApplyLayout() {
    Place(g_h_target, L.eTarget);
    Place(g_h_interval, L.eInterval);
    Place(g_h_reload, L.bApply);
    Place(g_h_start, L.bPause);
    Place(g_h_clear, L.bClear);
    Place(g_h_exit, L.bExit);
    Place(g_h_log, L.logEdit);
}

// ─── fonts ──────────────────────────────────────────────────
static HFONT MkFont(int px, int weight, const wchar_t* face) {
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       0, 0, CLEARTYPE_QUALITY, 0, face);
}

static void SetupLogControl() {
    if (!g_h_log) return;
    SendMessageW(g_h_log, EM_SETBKGNDCOLOR, 0, (LPARAM)theme::logBg);
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_EFFECTS;
    cf.dwEffects = 0;
    cf.yHeight = 195;   // 9.75pt
    cf.crTextColor = theme::logText;
    wcscpy(cf.szFaceName, L"Consolas");
    SendMessageW(g_h_log, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(g_h_log, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
}

static void MakeFonts() {
    HFONT* all[] = { &g_f_ui, &g_f_small, &g_f_semi, &g_f_value, &g_f_title, &g_f_logo, &g_f_btn };
    for (auto f : all) if (*f) { DeleteObject(*f); *f = nullptr; }

    g_f_ui    = MkFont(14, FW_NORMAL,   L"Segoe UI");
    g_f_small = MkFont(12, FW_NORMAL,   L"Segoe UI");
    g_f_semi  = MkFont(12, FW_SEMIBOLD, L"Segoe UI");
    g_f_value = MkFont(26, FW_SEMIBOLD, L"Segoe UI");
    g_f_title = MkFont(19, FW_BOLD,     L"Segoe UI");
    g_f_logo  = MkFont(19, FW_BOLD,     L"Segoe UI");
    g_f_btn   = MkFont(13, FW_SEMIBOLD, L"Segoe UI");

    if (g_h_target)   SendMessageW(g_h_target,   WM_SETFONT, (WPARAM)g_f_ui, TRUE);
    if (g_h_interval) SendMessageW(g_h_interval, WM_SETFONT, (WPARAM)g_f_ui, TRUE);
    SetupLogControl();
}

// ─── status model ───────────────────────────────────────────
enum State { ST_WAIT = 0, ST_OK = 1, ST_FAIL = 2, ST_PAUSED = 3 };

static State GetState(const LastResult& lr) {
    if (!g_active) return ST_PAUSED;
    if (!lr.has_result) return ST_WAIT;
    return lr.ok ? ST_OK : ST_FAIL;
}

static COLORREF LatencyColor(DWORD ms) {
    if (ms < 150) return theme::success;
    if (ms < 400) return theme::warning;
    return theme::danger;
}

static std::wstring StatusText(State s, const LastResult& lr) {
    switch (s) {
    case ST_PAUSED: return L"Paused";
    case ST_WAIT:   return L"Starting\u2026";
    case ST_OK:     return wfmt(L"Online  \u00B7  %lu ms", lr.latency);
    default:        return lr.error == L"no interface" ? L"No network" : L"Offline";
    }
}

static COLORREF StateColor(State s) {
    switch (s) {
    case ST_PAUSED: return theme::warning;
    case ST_WAIT:   return theme::muted;
    case ST_OK:     return theme::success;
    default:        return theme::danger;
    }
}

// ─── cards + graph ──────────────────────────────────────────
static void DrawCard(Painter& P, const RECT& rc, const wchar_t* title,
                     const std::wstring& value, const std::wstring& sub,
                     COLORREF valCol, int pct = -1, COLORREF barCol = theme::accent) {
    FillRound(P, rc, S(12), theme::surface, 255, theme::border);
    int ip = S(14);

    RECT t = Rc(rc.left + ip, rc.top + S(12), rc.right - ip, rc.top + S(28));
    Txt(P, title, t, g_f_semi, theme::muted, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    RECT v = Rc(rc.left + ip, rc.top + S(30), rc.right - ip, rc.top + S(62));
    Txt(P, value, v, g_f_value, valCol, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    if (pct >= 0) {
        int bx0 = rc.left + ip, bx1 = rc.right - ip, by = rc.top + S(70), bh = S(5);
        RECT track = Rc(bx0, by, bx1, by + bh);
        FillRound(P, track, bh / 2 + 1, theme::surface2);
        int fw = (int)((bx1 - bx0) * (pct / 100.0));
        if (fw > bh) {
            RECT fill = Rc(bx0, by, bx0 + fw, by + bh);
            FillRound(P, fill, bh / 2 + 1, barCol);
        }
    } else {
        RECT s = Rc(rc.left + ip, rc.top + S(64), rc.right - ip, rc.top + S(82));
        Txt(P, sub, s, g_f_small, theme::muted, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

static void DrawGraph(Painter& P, const RECT& rc, const std::vector<int>& h) {
    FillRound(P, rc, S(12), theme::surface, 255, theme::border);
    int ip = S(16);

    int maxv = 0;
    for (int v : h) if (v > maxv) maxv = v;
    int scaleMax = (std::max)(100, ((int)(maxv * 1.25) + 49) / 50 * 50);

    RECT t = Rc(rc.left + ip, rc.top + S(12), rc.left + ip + S(200), rc.top + S(28));
    Txt(P, L"LATENCY HISTORY", t, g_f_semi, theme::muted, DT_LEFT | DT_VCENTER);
    RECT t2 = Rc(rc.right - ip - S(200), rc.top + S(12), rc.right - ip, rc.top + S(28));
    Txt(P, wfmt(L"0 \u2013 %d ms  \u00B7  last %d pings", scaleMax, (int)h.size()),
        t2, g_f_small, theme::muted, DT_RIGHT | DT_VCENTER);

    RECT plot = Rc(rc.left + ip, rc.top + S(38), rc.right - ip, rc.bottom - S(14));
    float pw = (float)(plot.right - plot.left), ph = (float)(plot.bottom - plot.top);

    Pen grid(GC(theme::border, 150), 1.0f);
    for (int i = 0; i <= 3; ++i) {
        float gy = plot.top + ph * i / 3.0f;
        P.g.DrawLine(&grid, (float)plot.left, gy, (float)plot.right, gy);
    }

    if (h.size() < 2) {
        Txt(P, L"Collecting data\u2026", plot, g_f_small, theme::muted, DT_CENTER | DT_VCENTER);
        return;
    }

    float step = pw / (float)(MAX_HIST - 1);
    int n = (int)h.size();
    auto X = [&](int i) { return plot.right - (n - 1 - i) * step; };
    auto Y = [&](int v) {
        float f = (std::min)(1.0f, (float)v / (float)scaleMax);
        return plot.bottom - f * ph;
    };

    Pen linePen(GC(theme::accent), (float)S(2));
    linePen.SetLineJoin(LineJoinRound);
    Pen failPen(GC(theme::danger, 110), (float)S(2));
    LinearGradientBrush area(PointF(0, (float)plot.top), PointF(0, (float)plot.bottom),
                             GC(theme::accent, 80), GC(theme::accent, 0));

    std::vector<PointF> run;
    auto flush = [&]() {
        if (run.size() >= 2) {
            std::vector<PointF> poly = run;
            poly.push_back(PointF(run.back().X, (float)plot.bottom));
            poly.push_back(PointF(run.front().X, (float)plot.bottom));
            P.g.FillPolygon(&area, poly.data(), (INT)poly.size());
            P.g.DrawLines(&linePen, run.data(), (INT)run.size());
        } else if (run.size() == 1) {
            SolidBrush b(GC(theme::accent));
            float r = (float)S(2);
            P.g.FillEllipse(&b, run[0].X - r, run[0].Y - r, r * 2, r * 2);
        }
        run.clear();
    };

    for (int i = 0; i < n; ++i) {
        if (h[i] < 0) {
            flush();
            P.g.DrawLine(&failPen, X(i), (float)plot.top, X(i), (float)plot.bottom);
        } else {
            run.push_back(PointF(X(i), Y(h[i])));
        }
    }
    flush();

    if (h.back() >= 0) {
        float cx = X(n - 1), cy = Y(h.back());
        float r = (float)S(4);
        SolidBrush ring(GC(theme::accent, 70));
        SolidBrush dot(GC(theme::accent));
        P.g.FillEllipse(&ring, cx - r * 1.8f, cy - r * 1.8f, r * 3.6f, r * 3.6f);
        P.g.FillEllipse(&dot, cx - r * 0.8f, cy - r * 0.8f, r * 1.6f, r * 1.6f);
    }
}

// ─── full window paint ──────────────────────────────────────
static void PaintWindow(HDC hdc, const RECT& client) {
    LastResult lr;
    std::vector<int> hist;
    {
        std::lock_guard<std::mutex> lk(g_last_mtx);
        lr = g_last;
        hist = g_hist;
    }
    Config cfg = GetCfg();
    State st = GetState(lr);

    DWORD sent = g_sent.load(), failed = g_failed.load();
    DWORD okc = (sent >= failed) ? (sent - failed) : 0;
    int pct = sent ? (int)((okc * 100) / sent) : 100;

    int mn = INT_MAX, mx = 0; long long sum = 0; int cnt = 0;
    for (int v : hist) if (v >= 0) { mn = (std::min)(mn, v); mx = (std::max)(mx, v); sum += v; ++cnt; }

    Painter P(hdc);
    FillRect(hdc, &client, g_bg_brush);

    // header: logo
    {
        float x = (float)L.logo.left, y = (float)L.logo.top;
        float w = (float)(L.logo.right - L.logo.left), h = (float)(L.logo.bottom - L.logo.top);
        GraphicsPath path; RoundPath(path, x, y, w, h, (float)S(10));
        LinearGradientBrush gb(PointF(x, y), PointF(x + w, y + h),
                               GC(RGB(120, 163, 255)), GC(RGB(84, 104, 240)));
        P.g.FillPath(&gb, &path);
        Txt(P, L"L", L.logo, g_f_logo, RGB(255, 255, 255), DT_CENTER | DT_VCENTER);
    }
    // header: title + subtitle
    {
        int tx = L.logo.right + S(12);
        RECT t = Rc(tx, L.header.top + S(2), L.pill.left - S(12), L.header.top + S(26));
        Txt(P, L"Latenzy", t, g_f_title, theme::text, DT_LEFT | DT_VCENTER);
        RECT s = Rc(tx, L.header.top + S(26), L.pill.left - S(12), L.header.top + S(46));
        Txt(P, L"Monitoring  " + cfg.target_url, s, g_f_small, theme::muted,
            DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS);
    }
    // header: status pill
    {
        COLORREF sc = StateColor(st);
        FillRound(P, L.pill, (L.pill.bottom - L.pill.top) / 2, sc, 28, sc, 1.0f);
        float d = (float)S(8);
        float cx = L.pill.left + S(16), cy = (L.pill.top + L.pill.bottom) / 2.0f;
        SolidBrush halo(GC(sc, 60)), dot(GC(sc));
        P.g.FillEllipse(&halo, cx - d, cy - d, d * 2, d * 2);
        P.g.FillEllipse(&dot, cx - d / 2, cy - d / 2, d, d);
        RECT t = Rc(L.pill.left + S(32), L.pill.top, L.pill.right - S(12), L.pill.bottom);
        Txt(P, StatusText(st, lr), t, g_f_semi, sc, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // input fields
    {
        HWND focus = GetFocus();
        FillRound(P, L.fTarget, S(10), theme::surface2, 255,
                  focus == g_h_target ? theme::accent : theme::border);
        FillRound(P, L.fInterval, S(10), theme::surface2, 255,
                  focus == g_h_interval ? theme::accent : theme::border);

        RECT l1 = Rc(L.fTarget.left + S(14), L.fTarget.top, L.fTarget.left + S(52), L.fTarget.bottom);
        Txt(P, L"URL", l1, g_f_semi, theme::muted, DT_LEFT | DT_VCENTER);
        RECT l2 = Rc(L.fInterval.left + S(14), L.fInterval.top, L.fInterval.left + S(56), L.fInterval.bottom);
        Txt(P, L"Every", l2, g_f_semi, theme::muted, DT_LEFT | DT_VCENTER);
        RECT l3 = Rc(L.eInterval.right + S(4), L.fInterval.top, L.fInterval.right - S(8), L.fInterval.bottom);
        Txt(P, L"sec", l3, g_f_semi, theme::muted, DT_LEFT | DT_VCENTER);
    }

    // metric cards
    {
        std::wstring val, sub; COLORREF vc = theme::muted;
        switch (st) {
        case ST_PAUSED: val = L"Paused";  sub = L"Monitoring is paused"; vc = theme::warning; break;
        case ST_WAIT:   val = L"\u2014";  sub = L"Waiting for first result"; break;
        case ST_OK:     val = wfmt(L"%lu ms", lr.latency); sub = wfmt(L"HTTP %lu", lr.code); vc = LatencyColor(lr.latency); break;
        default:
            val = L"Failed"; vc = theme::danger;
            sub = lr.error.empty() ? wfmt(L"HTTP %lu", lr.code) : lr.error; break;
        }
        DrawCard(P, L.card[0], L"CURRENT LATENCY", val, sub, vc);

        if (cnt > 0)
            DrawCard(P, L.card[1], L"AVERAGE", wfmt(L"%d ms", (int)(sum / cnt)),
                     wfmt(L"min %d  \u00B7  max %d", mn, mx), LatencyColor((DWORD)(sum / cnt)));
        else
            DrawCard(P, L.card[1], L"AVERAGE", L"\u2014", L"No samples yet", theme::muted);

        DrawCard(P, L.card[2], L"PACKETS SENT", wfmt(L"%lu", sent),
                 wfmt(L"%lu failed", failed), theme::text);

        COLORREF rc3 = (pct > 95) ? theme::success : ((pct > 80) ? theme::warning : theme::danger);
        DrawCard(P, L.card[3], L"SUCCESS RATE", wfmt(L"%d%%", pct), L"", rc3, pct, rc3);
    }

    DrawGraph(P, L.graph, hist);

    // log label + container
    Txt(P, L"ACTIVITY", L.logLabel, g_f_semi, theme::muted, DT_LEFT | DT_VCENTER);
    FillRound(P, L.logBox, S(12), theme::logBg, 255, theme::border);

    // footer hint
    Txt(P, L"Closing the window keeps Latenzy running in the system tray.",
        L.footerText, g_f_small, theme::muted, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    P.g.Flush(FlushIntentionSync);
}

// ─── owner-drawn flat buttons ───────────────────────────────
static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_MOUSEMOVE:
        if (g_hover != h) {
            g_hover = h;
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hover == h) { g_hover = nullptr; InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, BtnProc, 0);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static void DrawButton(const DRAWITEMSTRUCT* d) {
    RECT rc = d->rcItem;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC mem = CreateCompatibleDC(d->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(d->hDC, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    bool hot = (g_hover == d->hwndItem);
    bool down = (d->itemState & ODS_SELECTED) != 0;
    bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);

    enum Kind { Primary, Secondary, Ghost, GhostDanger } kind = Secondary;
    switch (d->CtlID) {
    case ID_BTN_RELOAD: kind = Primary; break;
    case ID_BTN_START:  kind = g_active ? Secondary : Primary; break;
    case ID_BTN_CLEAR:  kind = Ghost; break;
    case ID_BTN_EXIT:   kind = GhostDanger; break;
    }

    COLORREF fill = CLR_INVALID, border = CLR_INVALID, fg = theme::text;
    switch (kind) {
    case Primary:
        fill = down ? theme::accentPress : (hot ? theme::accentHover : theme::accent);
        fg = RGB(255, 255, 255);
        break;
    case Secondary:
        fill = down ? theme::surface : (hot ? theme::surfaceHover : theme::surface2);
        border = theme::border;
        break;
    case Ghost:
        if (hot || down) fill = down ? theme::surface : theme::surface2;
        fg = hot ? theme::text : theme::muted;
        break;
    case GhostDanger:
        if (hot || down) fill = down ? theme::surface : theme::surface2;
        fg = hot ? theme::danger : theme::muted;
        break;
    }
    if (focus) border = theme::accent;

    {
        Painter P(mem);
        RECT local = { 0, 0, w, h };
        FillRect(mem, &local, g_bg_brush);
        if (fill != CLR_INVALID || border != CLR_INVALID)
            FillRound(P, local, S(10), fill, 255, border);

        wchar_t text[64] = {};
        GetWindowTextW(d->hwndItem, text, 63);
        Txt(P, text, local, g_f_btn, fg, DT_CENTER | DT_VCENTER);
    }

    BitBlt(d->hDC, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ─── app / tray icons ───────────────────────────────────────
static HICON MakeIcon(COLORREF c) {
    Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    GraphicsPath p; RoundPath(p, 1, 1, 30, 30, 8);
    SolidBrush b(GC(c));
    g.FillPath(&b, &p);
    Pen pen(Color(255, 255, 255, 255), 3.6f);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    PointF z[] = { PointF(11, 8), PointF(11, 23), PointF(23, 23) };
    g.DrawLines(&pen, z, 3);
    HICON h = nullptr;
    bmp.GetHICON(&h);
    return h;
}

static void CreateIcons() {
    g_icons[ST_WAIT]   = MakeIcon(theme::accent);
    g_icons[ST_OK]     = MakeIcon(RGB(34, 170, 120));
    g_icons[ST_FAIL]   = MakeIcon(RGB(226, 85, 85));
    g_icons[ST_PAUSED] = MakeIcon(RGB(214, 158, 20));
}

static void CreateTrayIcon() {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_icons[ST_WAIT] ? g_icons[ST_WAIT] : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"Latenzy");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void UpdateTray(State st, const std::wstring& tip) {
    if ((int)st == g_tray_state && tip == g_tray_tip) return;
    g_tray_state = (int)st;
    g_tray_tip = tip;
    g_nid.uFlags = NIF_ICON | NIF_TIP;
    if (g_icons[st]) g_nid.hIcon = g_icons[st];
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    g_nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (g_icons[st]) {
        SendMessageW(g_hwnd, WM_SETICON, ICON_BIG,   (LPARAM)g_icons[st]);
        SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icons[st]);
    }
}

static void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

// ─── control creation ───────────────────────────────────────
static HWND MkChild(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

static HWND MkButton(const wchar_t* text, int id) {
    HWND b = MkChild(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id);
    SetWindowSubclass(b, BtnProc, 0, 0);
    return b;
}

static void CreateControls() {
    LoadLibraryW(L"Msftedit.dll");

    g_h_target   = MkChild(L"EDIT", GetCfg().target_url.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, ID_EDIT_TARGET);
    g_h_interval = MkChild(L"EDIT", std::to_wstring(GetCfg().interval_seconds).c_str(),
                           ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, ID_EDIT_INTERVAL);
    SendMessageW(g_h_interval, EM_LIMITTEXT, 4, 0);
    SendMessageW(g_h_target, EM_LIMITTEXT, 500, 0);

    g_h_reload = MkButton(L"Apply", ID_BTN_RELOAD);
    g_h_start  = MkButton(L"Pause", ID_BTN_START);
    g_h_clear  = MkButton(L"Clear log", ID_BTN_CLEAR);
    g_h_exit   = MkButton(L"Exit", ID_BTN_EXIT);

    g_h_log = MkChild(L"RICHEDIT50W", L"",
        WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL, ID_LOG_EDIT);
    if (g_h_log) SetWindowTheme(g_h_log, L"DarkMode_Explorer", nullptr);
}

// ─── log drain (colored, smart auto-scroll) ─────────────────
static void AppendColored(const std::wstring& s, COLORREF color) {
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_EFFECTS;
    cf.dwEffects = 0;
    cf.crTextColor = color;
    SendMessageW(g_h_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_h_log, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_h_log, EM_REPLACESEL, FALSE, (LPARAM)s.c_str());
}

static void DrainLog() {
    std::vector<LogEntry> batch;
    {
        std::lock_guard<std::mutex> lk(g_log_mtx);
        for (size_t i = g_log_consumed; i < g_logs.size(); ++i) batch.push_back(g_logs[i]);
        g_log_consumed = g_logs.size();
    }
    if (batch.empty() || !g_h_log) return;

    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    GetScrollInfo(g_h_log, SB_VERT, &si);
    bool atBottom = (si.nMax == 0) || (si.nPos + (int)si.nPage >= si.nMax - 2);

    SendMessageW(g_h_log, WM_SETREDRAW, FALSE, 0);
    for (auto& e : batch) {
        COLORREF c = theme::logText;
        if (e.level == 0) c = theme::muted;
        else if (e.level == 2) c = theme::danger;
        else if (e.level == 3) c = theme::warning;
        AppendColored(e.ts + L"   ", theme::muted);
        AppendColored(e.msg + L"\n", c);
    }

    if (SendMessageW(g_h_log, EM_GETLINECOUNT, 0, 0) > 1500) {
        LRESULT idx = SendMessageW(g_h_log, EM_LINEINDEX, 300, 0);
        SendMessageW(g_h_log, EM_SETSEL, 0, idx);
        SendMessageW(g_h_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }

    SendMessageW(g_h_log, WM_SETREDRAW, TRUE, 0);
    if (atBottom) SendMessageW(g_h_log, WM_VSCROLL, SB_BOTTOM, 0);
    InvalidateRect(g_h_log, nullptr, TRUE);
}

// ─── commands ───────────────────────────────────────────────
static void ToggleActive() {
    bool now = !g_active.load();
    g_active.store(now);
    SetWindowTextW(g_h_start, now ? L"Pause" : L"Resume");
    InvalidateRect(g_h_start, nullptr, FALSE);
    PushLog(now ? L"resumed" : L"paused", 0);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ApplyConfig() {
    wchar_t buf[512] = {};
    GetWindowTextW(g_h_target, buf, 511);
    std::wstring new_target = buf;
    while (!new_target.empty() && iswspace(new_target.back())) new_target.pop_back();
    while (!new_target.empty() && iswspace(new_target.front())) new_target.erase(new_target.begin());
    if (new_target.empty()) new_target = L"https://www.google.com";
    if (new_target.find(L"://") == std::wstring::npos) new_target = L"https://" + new_target;

    BOOL ok = FALSE;
    int new_interval = GetDlgItemInt(g_hwnd, ID_EDIT_INTERVAL, &ok, FALSE);
    if (!ok || new_interval < 1) new_interval = 1;
    if (new_interval > 3600) new_interval = 3600;

    {
        std::lock_guard<std::mutex> lk(g_cfg_mtx);
        g_cfg.target_url = new_target;
        g_cfg.interval_seconds = new_interval;
    }

    SetWindowTextW(g_h_target, new_target.c_str());
    SetWindowTextW(g_h_interval, std::to_wstring(new_interval).c_str());

    SaveConfig();
    g_kick = true;   // ping immediately with new settings
    PushLog(wfmt(L"config applied: %s every %ds", new_target.c_str(), new_interval), 0);
    SetFocus(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void ClearLog() {
    SetWindowTextW(g_h_log, L"");
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_logs.clear();
    g_log_consumed = 0;
}

static void ShowTrayMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, L"Show window");
    AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE, g_active ? L"Pause monitoring" : L"Resume monitoring");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD, L"Apply config");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void ApplyDarkTitleBar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *PFN)(HWND, DWORD, LPCVOID, DWORD);
    PFN fn = (PFN)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        BOOL dark = TRUE;
        fn(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF cap = theme::bg, txt = theme::text, brd = theme::bg;
        fn(hwnd, 35, &cap, sizeof(cap));   // caption colour   (Win11)
        fn(hwnd, 36, &txt, sizeof(txt));   // caption text     (Win11)
        fn(hwnd, 34, &brd, sizeof(brd));   // border colour    (Win11)
    }
    FreeLibrary(dwm);
}

// ─── window proc ────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        typedef UINT (WINAPI *PFN_GDFW)(HWND);
        if (auto fn = (PFN_GDFW)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"))
            g_dpi = fn(hwnd);
        g_bg_brush   = CreateSolidBrush(theme::bg);
        g_edit_brush = CreateSolidBrush(theme::surface2);
        ApplyDarkTitleBar(hwnd);
        CreateControls();
        MakeFonts();
        CreateTrayIcon();
        SetTimer(hwnd, TIMER_UI, 250, nullptr);
        PushLog(L"Latenzy started", 0);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        RECT r = { 0, 0, S(640), S(560) };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = LOWORD(wParam);
        MakeFonts();
        RECT* r = (RECT*)lParam;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) return 0;
        ComputeLayout(LOWORD(lParam), HIWORD(lParam));
        ApplyLayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (wParam == TIMER_UI) {
            DrainLog();
            LastResult lr;
            { std::lock_guard<std::mutex> lk(g_last_mtx); lr = g_last; }
            State st = GetState(lr);
            UpdateTray(st, L"Latenzy \u2014 " + StatusText(st, lr));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, theme::surface2);
        SetTextColor(hdc, theme::text);
        return (LRESULT)g_edit_brush;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, theme::bg);
        SetTextColor(hdc, theme::text);
        return (LRESULT)g_bg_brush;
    }

    case WM_DRAWITEM:
        DrawButton((const DRAWITEMSTRUCT*)lParam);
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        PaintWindow(mem, client);
        BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);   // click on background drops edit focus
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_EDIT_TARGET:
        case ID_EDIT_INTERVAL:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS) {
                RECT a = L.fTarget, b = L.fInterval;
                InflateRect(&a, 2, 2); InflateRect(&b, 2, 2);
                InvalidateRect(hwnd, &a, FALSE);
                InvalidateRect(hwnd, &b, FALSE);
            }
            break;
        case IDOK:          ApplyConfig(); break;
        case IDCANCEL:      break;
        case ID_BTN_START:  ToggleActive(); break;
        case ID_BTN_RELOAD: ApplyConfig();  break;
        case ID_BTN_CLEAR:  ClearLog();     break;
        case ID_BTN_EXIT:
            g_running = false;
            DestroyWindow(hwnd);
            break;
        case ID_TRAY_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            break;
        case ID_TRAY_TOGGLE: ToggleActive(); break;
        case ID_TRAY_RELOAD: ApplyConfig();  break;
        case ID_TRAY_EXIT:
            g_running = false;
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) ShowTrayMenu();
        else if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── entry ──────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    // Per-monitor DPI awareness (v2) when available
    typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
    if (auto fn = (PFN_SPDAC)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        fn((HANDLE)-4);
    else
        SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icex);

    GdiplusStartupInput gsi;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gsi, nullptr);
    CreateIcons();

    LoadConfig();

    {   // initial DPI for sizing before the window exists
        HDC sdc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
        ReleaseDC(nullptr, sdc);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon         = g_icons[ST_WAIT];
    wc.hIconSm       = g_icons[ST_WAIT];
    wc.lpszClassName = L"LatenzyGuiClass";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT rc = { 0, 0, S(780), S(700) };
    AdjustWindowRectEx(&rc, style, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, L"LatenzyGuiClass", L"Latenzy",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    bool startInTray = cmdLine && wcsstr(cmdLine, L"--tray");
    if (!startInTray) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    std::thread worker(PingWorker);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = false;
    worker.join();
    SaveConfig();

    HFONT* fonts[] = { &g_f_ui, &g_f_small, &g_f_semi, &g_f_value, &g_f_title, &g_f_logo, &g_f_btn };
    for (auto f : fonts) if (*f) DeleteObject(*f);
    if (g_bg_brush)   DeleteObject(g_bg_brush);
    if (g_edit_brush) DeleteObject(g_edit_brush);
    for (auto ic : g_icons) if (ic) DestroyIcon(ic);

    GdiplusShutdown(gdiplusToken);
    return 0;
}
