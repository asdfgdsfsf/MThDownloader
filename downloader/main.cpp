// ============================================================================
//  MThDownloader —— 简洁多线程下载器 (Win32 原生 GUI + WinHTTP)
//
//  作者: Nikolabest
//
//  特性:
//   * 自适应线程: 增益探测 —— 周期性加倍线程, 总速度明显提升(有余量)则继续加,
//     提升微弱(带宽饱和)则维持, 并周期性重新试探以捕捉带宽恢复
//   * 小文件保守: 文件小于阈值时直接以保守线程数下载, 避免试探浪费时间
//   * 所有阈值界面可调, 下载中实时生效
//   * 分块动态队列 + 失败重试 + 暂停/继续 + 取消
//   * BT 种子/磁力下载: 内置 aria2c 引擎 (DHT/PEX/uTP), JSON-RPC 驱动, 原生界面
//   * 性能: 文件预分配、每线程独立句柄顺序写、原子计数、UI 定时器集中刷新
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cwctype>
#include <ctime>

// ---------------------------- 控件 ID ---------------------------------------
enum {
    IDC_URL = 1001, IDC_PATH, IDC_BROWSE, IDC_START, IDC_PAUSE, IDC_CANCEL,
    IDC_MAXTH, IDC_INITTH, IDC_SPEEDTH, IDC_SIZETH, IDC_INTERVAL, IDC_SPLITMIN,
    IDC_PROGRESS, IDC_STATUS, IDC_LOG,
    IDC_TAB, IDC_BT_SRC, IDC_BT_BROWSE, IDC_BT_PATH, IDC_BT_DIRBROWSE,
    IDC_LURL, IDC_LPATH, IDC_LSRC, IDC_LBTDIR, IDC_ADAPTGRP,
    IDC_LAD1, IDC_LAD2, IDC_LAD3, IDC_LAD4, IDC_LAD5, IDC_LAD6,
};

// ---------------------------- 任务状态 --------------------------------------
enum JobState {
    ST_IDLE = 0, ST_PROBING, ST_RUNNING, ST_PAUSED, ST_DONE, ST_FAILED, ST_CANCELED,
};

// ---------------------------- 自定义消息 ------------------------------------
#define WM_APP_LOG       (WM_APP + 1)   // lParam = new std::wstring
#define WM_APP_STATE     (WM_APP + 2)   // wParam = JobState
#define WM_APP_AUTOSTART (WM_APP + 3)   // 命令行 URL 到达, 启动自动下载
#define WM_COPYDATA_URL  1              // WM_COPYDATA dwData 标记: 单实例转发

static int g_mode = 0;    // 0 = HTTP, 1 = BT
static std::wstring g_cmdUrl, g_cmdDir;   // 命令行传入的 URL 与可选保存目录

static const double KB = 1024.0, MB = KB * 1024.0;

// ---------------------------- 小工具 ----------------------------------------
static std::wstring FmtBytes(uint64_t n) {
    wchar_t b[64];
    if (n < 1024ull) swprintf(b, 64, L"%llu B", (unsigned long long)n);
    else if (n < 1024ull * 1024) swprintf(b, 64, L"%.1f KB", n / KB);
    else if (n < 1024ull * 1024 * 1024) swprintf(b, 64, L"%.2f MB", n / MB);
    else swprintf(b, 64, L"%.2f GB", n / (MB * 1024));
    return b;
}

static std::wstring FmtSpeed(double bps) {
    wchar_t b[64];
    if (bps < KB) swprintf(b, 64, L"%.0f B/s", bps);
    else if (bps < MB) swprintf(b, 64, L"%.1f KB/s", bps / KB);
    else swprintf(b, 64, L"%.2f MB/s", bps / MB);
    return b;
}

// 速度双单位: MB/s (文件体积) + Mb/s (网络带宽, 十进制)
static std::wstring SpeedBoth(double bps) {
    wchar_t b[96];
    swprintf(b, 96, L"%ls (%.1f Mb/s)", FmtSpeed(bps).c_str(), bps * 8.0 / 1000000.0);
    return b;
}

static std::wstring FmtDur(uint64_t sec) {
    wchar_t b[64];
    if (sec >= 3600) swprintf(b, 64, L"%u:%02u:%02u",
        (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    else swprintf(b, 64, L"%02u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
    return b;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// URL 百分号解码 (含 UTF-8 还原)
static std::wstring PercentDecode(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t ch = s[i];
        if (ch == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            wchar_t hex[3] = { s[i + 1], s[i + 2], 0 };
            out.push_back((char)wcstoul(hex, nullptr, 16));
            i += 2;
        } else if (ch == L'+') {
            out.push_back(' ');
        } else {
            out.push_back((char)ch);
        }
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, &w[0], n);
    return w;
}

static std::wstring SanitizeName(std::wstring s) {
    for (auto& c : s)
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*' || (int)c < 32)
            c = L'_';
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    if (s.size() > 120) s.resize(120);
    return s;
}

static bool ParseUintStr(const std::wstring& s, uint64_t& v) {
    if (s.empty()) return false;
    wchar_t* e = nullptr;
    unsigned long long r = wcstoull(s.c_str(), &e, 10);
    if (e == s.c_str()) return false;
    v = r;
    return true;
}

// MIME → 常见扩展名 (文件名无后缀时自动补全)
static std::wstring ExtFromMime(const std::wstring& m) {
    std::wstring t = Lower(m);
    size_t sc = t.find(L';');
    if (sc != std::wstring::npos) t.resize(sc);
    t = Trim(t);
    static const struct { const wchar_t* mime; const wchar_t* ext; } tab[] = {
        { L"application/zip", L".zip" },
        { L"application/x-zip-compressed", L".zip" },
        { L"application/gzip", L".gz" },
        { L"application/x-gzip", L".gz" },
        { L"application/x-tar", L".tar" },
        { L"application/x-7z-compressed", L".7z" },
        { L"application/x-rar-compressed", L".rar" },
        { L"application/vnd.android.package-archive", L".apk" },
        { L"application/x-msdownload", L".exe" },
        { L"application/x-msi", L".msi" },
        { L"application/pdf", L".pdf" },
        { L"video/mp4", L".mp4" },
        { L"video/x-matroska", L".mkv" },
        { L"audio/mpeg", L".mp3" },
        { L"image/png", L".png" },
        { L"image/jpeg", L".jpg" },
        { L"image/gif", L".gif" },
        { L"text/plain", L".txt" },
        { L"application/json", L".json" },
    };
    for (auto& e : tab) if (t == e.mime) return e.ext;
    return L"";
}

// ============================================================================
//  分块定义
// ============================================================================
struct Block {
    uint64_t beg = 0;    // 起始字节 (含)
    uint64_t end = 0;    // 结束字节 (含)
    uint64_t pos = 0;    // 已下载到的位置
    int      fails = 0;  // 连续失败次数
};

static std::wstring DefaultDownloadDir();   // 默认下载目录 (定义在文件后部)

// ---------------- 下载历史 (exe 旁 MThDownloader.history.jsonl, 供扩展面板) ----
static std::atomic<long long> g_histSeq{ 0 };
static void HistAppend(const std::wstring& event, long long id,
                       const std::wstring& url, const std::wstring& file,
                       uint64_t size, int secs, const std::wstring& err);

// DownloadRange 返回值
enum { DR_OK = 0, DR_INTERRUPTED = 1, DR_ERROR = 2 };

// ============================================================================
//  下载器核心 (单例, 一次跑一个任务)
// ============================================================================
class Downloader {
public:
    static Downloader& Inst() { static Downloader d; return d; }

    // ---- 设置 (UI 线程写, 工作线程读, 全部原子) ----
    std::atomic<int>    maxThreads_{ 128 };   // 最大线程
    std::atomic<int>    initThreads_{ 8 };    // 初始/保守线程
    std::atomic<double> gainThPct_{ 10.0 };   // 扩容增益阈值 %, 加线程后提速超过它则继续加
    std::atomic<double> smallFileMB_{ 8.0 };  // 小文件阈值 MB, 低于则保守模式
    std::atomic<int>    intervalMs_{ 2000 };  // 测速间隔
    std::atomic<int>    splitMinKB_{ 512 };   // 尾部拆分下限 KB, 剩余小于它不再拆分

    // ---- 运行时状态 ----
    std::atomic<JobState> state_{ ST_IDLE };
    std::atomic<uint64_t> totalBytes_{ 0 }, doneBytes_{ 0 };
    std::atomic<int>      activeWorkers_{ 0 }, threadTarget_{ 0 };
    std::atomic<double>   curSpeed_{ 0.0 };
    std::atomic<bool>     ranged_{ true }, adaptive_{ true };
    std::atomic<int>      splitReq_{ 0 };     // 尾部均衡: 请求在跑块一分为二

    HWND ui_ = nullptr;
    std::wstring finalPath_;
    std::atomic<long long> histId_{ 0 };     // 下载历史记录 id
    std::atomic<unsigned long long> startTick_{ 0 };

    JobState St() const { return state_.load(std::memory_order_relaxed); }
    bool ranged() const { return ranged_.load(std::memory_order_relaxed); }

    void Log(const std::wstring& s) {
        if (!ui_) return;
        PostMessageW(ui_, WM_APP_LOG, 0, (LPARAM)(new std::wstring(s)));
    }

    // ---------------- 启动 ----------------
    void Start(const std::wstring& url, const std::wstring& savePath) {
        CleanupThreads();
        CloseSession();

        url_ = url;
        finalPath_ = savePath;
        partPath_ = savePath + L".part";

        doneBytes_ = 0; totalBytes_ = 0; curSpeed_ = 0.0;
        activeWorkers_ = 0; threadTarget_ = 0;
        cancel_ = false; paused_ = false;
        ranged_ = true; adaptive_ = true;
        {
            std::lock_guard<std::mutex> lk(qm_);
            pending_.clear();
            inflight_ = 0;
            failedBlocks_ = 0;
        }
        SetState(ST_PROBING);
        Log(L"任务开始: " + url);
        histId_ = ++g_histSeq;
        startTick_ = GetTickCount64();
        HistAppend(L"start", histId_.load(), url, L"", 0, 0, L"");
        try { probe_ = std::thread(&Downloader::ProbeEntry, this); }
        catch (...) { SetState(ST_FAILED); Log(L"创建线程失败"); }
    }

    // ---------------- 暂停 / 继续 ----------------
    void Pause() {
        if (St() == ST_RUNNING) { paused_ = true; SetState(ST_PAUSED); Log(L"已暂停"); }
    }
    void Resume() {
        if (St() != ST_PAUSED) return;
        paused_ = false;
        SetState(ST_RUNNING);
        Log(L"继续下载, 恢复 " + std::to_wstring(threadTarget_.load()) + L" 线程");
        SpawnWorkers(threadTarget_.load());
    }

    // ---------------- 取消 (异步清理) ----------------
    void Cancel() {
        JobState s = St();
        if (s != ST_PROBING && s != ST_RUNNING && s != ST_PAUSED) return;
        cancel_ = true;
        paused_ = false;
        SetState(ST_CANCELED);
        HistAppend(L"cancel", histId_.load(), url_, L"", 0, 0, L"");
        if (cleaner_.joinable()) cleaner_.join();  // 上一轮清理早已结束, 不会久等
        try {
            cleaner_ = std::thread([this] {
                CleanupThreads();
                CloseSession();
                DeleteFileW(partPath_.c_str());
                Log(L"已取消, 临时文件已删除");
                SetState(ST_IDLE);
            });
        } catch (...) {}
    }

    // ---------------- 退出前收尾 ----------------
    void Shutdown() {
        cancel_ = true; paused_ = false;
        if (St() == ST_RUNNING || St() == ST_PAUSED || St() == ST_PROBING)
            SetState(ST_CANCELED);
        if (cleaner_.joinable()) cleaner_.join();
        else CleanupThreads();
        CloseSession();
    }

private:
    Downloader() = default;
    ~Downloader() = default;

    // ---- URL/连接 ----
    std::wstring url_, urlPath_, host_;
    std::wstring partPath_;
    bool  secure_ = false;
    unsigned short port_ = 80;
    HINTERNET session_ = nullptr;

    // ---- 队列 ----
    std::mutex qm_;
    std::deque<Block> pending_;
    std::atomic<int> inflight_{ 0 };     // 在跑块数 (改动持 qm_ 锁, 读取可无锁)
    int  failedBlocks_ = 0;

    // ---- 控制 ----
    std::atomic<bool> cancel_{ false }, paused_{ false };
    std::thread probe_, ctrl_, cleaner_;
    std::vector<std::thread> workers_;

    void SetState(JobState s) {
        state_.store(s, std::memory_order_relaxed);
        if (ui_) PostMessageW(ui_, WM_APP_STATE, (WPARAM)s, 0);
    }

    // -------------------- 线程/会话生命周期 --------------------
    void CleanupThreads() {
        if (probe_.joinable()) probe_.join();
        if (ctrl_.joinable()) ctrl_.join();
        for (auto& t : workers_) if (t.joinable()) t.join();
        workers_.clear();
    }
    void CloseSession() {
        if (session_) { WinHttpCloseHandle(session_); session_ = nullptr; }
    }

    // -------------------- 探测线程 --------------------
    void ProbeEntry() {
        // 解析 URL
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = L"";
        wchar_t path[2048] = L"";
        uc.lpszHostName = host;        uc.dwHostNameLength = 255;
        uc.lpszUrlPath = path;         uc.dwUrlPathLength = 2047;
        if (!WinHttpCrackUrl(url_.c_str(), 0, 0, &uc)) {
            FailStart(L"URL 解析失败");
            return;
        }
        host_ = host;
        urlPath_ = path;
        secure_ = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        port_ = uc.nPort;

        session_ = WinHttpOpen(L"MThDownloader/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) { FailStart(L"WinHttpOpen 失败"); return; }
        WinHttpSetTimeouts(session_, 10000, 10000, 15000, 15000);

        // 用 Range:0-0 的 GET 一次拿到: 大小 / 是否支持分段 / 文件名
        uint64_t size = 0;
        bool ranged = false;
        std::wstring fname;
        std::wstring ctype;
        std::wstring err;
        if (!ProbeSize(size, ranged, fname, ctype, err)) { FailStart(err); return; }

        ranged_ = (ranged && size > 0);   // 无大小或无分段支持都按单流处理
        totalBytes_ = size;
        if (fname.empty()) fname = FileNameFromUrl();
        if (fname.empty()) fname = L"download.bin";
        // 服务器/URL 文件名无后缀 → 按 Content-Type 补全扩展名
        {
            size_t fdot = fname.find_last_of(L'.');
            if (fdot == std::wstring::npos || fdot == 0) {
                std::wstring e = ExtFromMime(ctype);
                if (!e.empty()) fname += e;
            }
        }

        // 保存路径 = 用户选的目录 + 强制自动填写的文件名 (来自服务器/URL)
        std::wstring dir = finalPath_;
        if (dir.empty()) dir = DefaultDownloadDir();
        if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        finalPath_ = dir + fname;
        partPath_ = finalPath_ + L".part";
        Log(L"文件: " + finalPath_);
        if (!ranged) Log(L"服务器不支持断点分段, 单线程下载");

        // 规划分块与线程
        int initTh = ClampInt(initThreads_, 1, 256);
        bool adaptive = false;
        if (ranged && size > 0) {
            double smallTh = smallFileMB_.load();
            adaptive = (double)size >= smallTh * MB;
            if (adaptive) {
                Log(L"自适应模式: " + FmtBytes(size) + L" >= 阈值 " +
                    TrimDouble(smallTh) + L" MB");
            } else {
                Log(L"保守模式: " + FmtBytes(size) + L" < 阈值 " +
                    TrimDouble(smallTh) + L" MB, 固定 " + std::to_wstring(initTh) + L" 线程");
            }
        }

        adaptive_ = adaptive;
        int nBlocks = 1;
        if (ranged && size > 0) {
            // 分块: 让块数 >= 最大线程的 4 倍, 保证扩线程时始终有活干;
            // 上限压小到 8MB, 末尾阶段也能多线程并行收尾
            int mt = ClampInt(maxThreads_, 1, 256);
            uint64_t bs = size / (uint64_t)(mt * 4);
            if (bs < 256 * 1024) bs = 256 * 1024;
            if (bs > 8ull * 1024 * 1024) bs = 8ull * 1024 * 1024;
            nBlocks = (int)((size + bs - 1) / bs);
            BuildBlocks(size, bs);
            // 预分配文件
            if (!PreAllocate(size)) { FailStart(L"无法创建本地文件: " + partPath_); return; }
        } else {
            // 单流: 一个特殊的"流式块"
            Block b; b.beg = 0; b.end = ~0ull; b.pos = 0;
            std::lock_guard<std::mutex> lk(qm_);
            pending_.push_back(b);
        }

        int nThreads = std::min(initTh, nBlocks);
        threadTarget_ = nThreads;
        Log(L"初始线程: " + std::to_wstring(nThreads) +
            (adaptive ? L" (速度低于阈值将自动加倍)" : L""));
        SetState(ST_RUNNING);
        SpawnWorkers(nThreads);
        try { ctrl_ = std::thread(&Downloader::ControllerEntry, this); } catch (...) {}
    }

    void FailStart(const std::wstring& msg) {
        Log(L"错误: " + msg);
        CloseSession();
        DeleteFileW(partPath_.c_str());
        SetState(ST_FAILED);
    }

    // 探测: GET Range: bytes=0-0
    bool ProbeSize(uint64_t& size, bool& ranged, std::wstring& fname,
                   std::wstring& ctype, std::wstring& err) {
        HINTERNET c = WinHttpConnect(session_, host_.c_str(), port_, 0);
        if (!c) { err = L"无法连接主机 " + host_; return false; }
        HINTERNET r = MakeRequest(c, L"Range: bytes=0-0\r\n");
        if (!r) { WinHttpCloseHandle(c); err = L"发送请求失败"; return false; }

        int status = QueryStatus(r);
        std::wstring raw = RawHeaders(r);
        // 读完残留 body (0-1 字节)
        char tmp[256]; DWORD rd = 0;
        while (WinHttpReadData(r, tmp, sizeof(tmp), &rd) && rd > 0) {}

        WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);

        if (status == 206) {
            ranged = true;
            size = ParseContentRangeTotal(raw);
        } else if (status == 200) {
            ranged = false;
            size = ParseHeaderUint(raw, L"content-length");
        } else if (status == 0) {
            err = L"网络错误, 无响应";
            return false;
        } else {
            err = L"服务器返回 HTTP " + std::to_wstring(status);
            return false;
        }
        fname = ParseDispositionFilename(raw);
        std::wstring ct;
        if (GetHeader(raw, L"content-type", ct)) {
            size_t semi = ct.find(L';');
            if (semi != std::wstring::npos) ct.resize(semi);
            ctype = Trim(ct);
        }
        return true;
    }

    HINTERNET MakeRequest(HINTERNET conn, const wchar_t* extraHeaders) {
        HINTERNET r = WinHttpOpenRequest(conn, L"GET", urlPath_.c_str(), NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            secure_ ? WINHTTP_FLAG_SECURE : 0);
        if (!r) return nullptr;
        if (extraHeaders && *extraHeaders)
            WinHttpAddRequestHeaders(r, extraHeaders, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(r, NULL)) {
            WinHttpCloseHandle(r);
            return nullptr;
        }
        return r;
    }

    static int QueryStatus(HINTERNET r) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        return (int)code;
    }

    static std::wstring RawHeaders(HINTERNET r) {
        DWORD len = 0;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX, NULL, &len, WINHTTP_NO_HEADER_INDEX);
        if (!len) return L"";
        std::wstring s(len / 2 + 1, L'\0');
        if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX, (LPVOID)s.data(), &len,
                WINHTTP_NO_HEADER_INDEX))
            return L"";
        s.resize(len / 2);
        return s;
    }

    // 从原始头里取指定头的值 (不区分大小写)
    static bool GetHeader(const std::wstring& raw, const std::wstring& nameL,
                          std::wstring& val) {
        size_t p = 0;
        while (p < raw.size()) {
            size_t e = raw.find(L'\n', p);
            if (e == std::wstring::npos) e = raw.size();
            std::wstring line = raw.substr(p, e - p);
            p = e + 1;
            size_t colon = line.find(L':');
            if (colon == std::wstring::npos) continue;   // 状态行等
            std::wstring key = Lower(Trim(line.substr(0, colon)));
            if (key == nameL) {
                val = Trim(line.substr(colon + 1));
                return true;
            }
        }
        return false;
    }

    static uint64_t ParseHeaderUint(const std::wstring& raw, const std::wstring& name) {
        std::wstring v;
        if (!GetHeader(raw, name, v)) return 0;
        uint64_t r = 0;
        ParseUintStr(v, r);
        return r;
    }

    // Content-Range: bytes 0-0/12345 → 12345
    static uint64_t ParseContentRangeTotal(const std::wstring& raw) {
        std::wstring v;
        if (!GetHeader(raw, L"content-range", v)) return 0;
        size_t slash = v.rfind(L'/');
        if (slash == std::wstring::npos) return 0;
        uint64_t r = 0;
        if (!ParseUintStr(v.substr(slash + 1), r)) return 0;
        return r;
    }

    // Content-Disposition 里的文件名 (优先 filename*=UTF-8'')
    static std::wstring ParseDispositionFilename(const std::wstring& raw) {
        std::wstring v;
        if (!GetHeader(raw, L"content-disposition", v)) return L"";
        std::wstring lv = Lower(v);
        size_t p = lv.find(L"filename*=utf-8''");
        if (p != std::wstring::npos) {
            std::wstring rest = v.substr(p + 17);
            size_t semi = rest.find(L';');
            if (semi != std::wstring::npos) rest.resize(semi);
            return SanitizeName(PercentDecode(Trim(rest)));
        }
        p = lv.find(L"filename=");
        if (p != std::wstring::npos) {
            std::wstring rest = v.substr(p + 9);
            size_t semi = rest.find(L';');
            if (semi != std::wstring::npos) rest.resize(semi);
            rest = Trim(rest);
            if (rest.size() >= 2 && rest.front() == L'"' && rest.back() == L'"')
                rest = rest.substr(1, rest.size() - 2);
            return SanitizeName(PercentDecode(rest));
        }
        return L"";
    }

    std::wstring FileNameFromUrl() {
        std::wstring dec = PercentDecode(urlPath_);
        size_t slash = dec.find_last_of(L'/');
        if (slash != std::wstring::npos) dec = dec.substr(slash + 1);
        return SanitizeName(dec);
    }

    static int ClampInt(std::atomic<int>& a, int lo, int hi) {
        int v = a.load();
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static std::wstring TrimDouble(double d) {
        wchar_t b[32];
        swprintf(b, 32, L"%.1f", d);
        return b;
    }

    void BuildBlocks(uint64_t size, uint64_t bs) {
        std::lock_guard<std::mutex> lk(qm_);
        pending_.clear();
        for (uint64_t b = 0; b < size; b += bs) {
            Block blk;
            blk.beg = b;
            blk.end = std::min(b + bs - 1, size - 1);
            blk.pos = b;
            pending_.push_back(blk);
        }
    }

    bool PreAllocate(uint64_t size) {
        DeleteFileW(partPath_.c_str());
        HANDLE h = CreateFileW(partPath_.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)size;
        if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) || !SetEndOfFile(h)) {
            CloseHandle(h);
            return false;
        }
        CloseHandle(h);
        return true;
    }

    void SpawnWorkers(int n) {
        for (int i = 0; i < n; ++i) {
            try { workers_.emplace_back(&Downloader::WorkerEntry, this); }
            catch (...) { break; }
        }
    }

    // -------------------- 工作线程 --------------------
    void WorkerEntry() {
        activeWorkers_++;
        HINTERNET conn = WinHttpConnect(session_, host_.c_str(), port_, 0);
        HANDLE hFile = INVALID_HANDLE_VALUE;

        while (!cancel_) {
            if (paused_) break;
            Block b;
            bool got = false;
            {
                std::lock_guard<std::mutex> lk(qm_);
                if (!pending_.empty()) {
                    b = pending_.front();
                    pending_.pop_front();
                    inflight_++;
                    got = true;
                }
            }
            if (!got) {
                // 队列暂时空但还有在跑块: 等尾部拆分块入队, 而不是提前退出,
                // 这样末尾阶段也能保持多线程
                if (inflight_.load() > 0 && !cancel_) { Sleep(150); continue; }
                break;
            }

            if (hFile == INVALID_HANDLE_VALUE)
                hFile = CreateFileW(partPath_.c_str(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);

            int res = DR_ERROR;
            for (int attempt = 1; attempt <= 3; ++attempt) {
                if (paused_ || cancel_) { res = DR_INTERRUPTED; break; }
                res = DownloadRange(conn, hFile, b);
                if (res != DR_ERROR) break;
                if (attempt < 3) {
                    Sleep(300 * attempt);
                    // 换新连接重试, 避免坏连接上连续失败
                    if (conn) WinHttpCloseHandle(conn);
                    conn = WinHttpConnect(session_, host_.c_str(), port_, 0);
                }
            }

            // 先关文件句柄再归还块: 完成判定会重命名文件,
            // 若任何句柄未关, inflight 清零后判定线程会抢先把重命名做失败
            if (hFile != INVALID_HANDLE_VALUE) {
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;
            }

            {
                std::lock_guard<std::mutex> lk(qm_);
                inflight_--;
                if (res == DR_INTERRUPTED) pending_.push_front(b);
                else if (res == DR_ERROR) {
                    if (++b.fails < 5) pending_.push_back(b);
                    else { failedBlocks_++; Log(L"分块多次失败, 放弃 [" +
                        std::to_wstring(b.beg) + L"]"); }
                }
            }
        }

        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        if (conn) WinHttpCloseHandle(conn);

        {
            std::lock_guard<std::mutex> lk(qm_);
            CheckCompletionLocked();
        }
        activeWorkers_--;
    }

    // 下载一个块 (从 b.pos 续传到 b.end)
    int DownloadRange(HINTERNET conn, HANDLE hFile, Block& b) {
        if (conn == nullptr || hFile == INVALID_HANDLE_VALUE) return DR_ERROR;
        bool streaming = (b.end == ~0ull);      // 未知大小的单流模式

        wchar_t hdr[64];
        HINTERNET r;
        if (streaming) {
            r = MakeRequest(conn, nullptr);
        } else {
            swprintf(hdr, 64, L"Range: bytes=%llu-%llu\r\n",
                (unsigned long long)b.pos, (unsigned long long)b.end);
            r = MakeRequest(conn, hdr);
        }
        if (!r) return DR_ERROR;

        int status = QueryStatus(r);
        // 分段下载要求 206; 200 只允许发生在整文件块且从 0 开始
        if (!streaming && status != 206 && !(status == 200 && b.pos == b.beg && b.beg == 0)) {
            WinHttpCloseHandle(r);
            return DR_ERROR;
        }
        if (streaming && status != 200 && status != 206) {
            WinHttpCloseHandle(r);
            return DR_ERROR;
        }

        std::vector<char> buf(256 * 1024);
        int ret = DR_ERROR;
        uint64_t pos = b.pos;

        for (;;) {
            if (cancel_ || paused_) { ret = DR_INTERRUPTED; break; }
            DWORD rd = 0;
            if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &rd)) {
                ret = DR_ERROR;
                break;
            }
            if (rd == 0) {                       // 连接正常结束
                ret = (streaming || pos >= b.end + 1) ? DR_OK : DR_ERROR;
                break;
            }
            DWORD toWrite = rd;
            if (!streaming) {                    // 防御: 越过块尾则截断
                uint64_t remain = b.end + 1 - pos;
                if (remain == 0) { ret = DR_OK; break; }
                toWrite = (DWORD)std::min<uint64_t>(rd, remain);
            }
            DWORD written = 0;
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)pos;
            if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) ||
                !WriteFile(hFile, buf.data(), toWrite, &written, NULL) ||
                written != toWrite) {
                ret = DR_ERROR;
                break;
            }
            pos += toWrite;
            b.pos = pos;
            doneBytes_ += toWrite;
            // 尾部均衡: 控制器请求拆分时, 把当前块剩余部分一分为二,
            // 一半放回队列, 让闲置线程并行收尾
            if (splitReq_.exchange(0) && !streaming) {
                uint64_t remain = b.end + 1 - pos;
                if (remain > (uint64_t)splitMinKB_.load() * 1024) {
                    uint64_t mid = pos + remain / 2;
                    Block nb;
                    nb.beg = mid; nb.end = b.end; nb.pos = mid;
                    b.end = mid - 1;
                    std::lock_guard<std::mutex> lk(qm_);
                    pending_.push_back(nb);
                }
            }
            if (!streaming && pos == b.end + 1) { ret = DR_OK; break; }
        }

        WinHttpCloseHandle(r);
        return ret;
    }

    // 完成判定 (须持 qm_ 锁; 此刻所有工作线程均已关闭文件句柄)
    void CheckCompletionLocked() {
        if (St() != ST_RUNNING) return;
        if (!pending_.empty() || inflight_ > 0) return;
        if (failedBlocks_ > 0) {
            CloseSession();
            DeleteFileW(partPath_.c_str());
            HistAppend(L"fail", histId_.load(), url_, L"",
                       doneBytes_.load(), 0, L"部分分块下载失败");
            SetState(ST_FAILED);
            return;
        }
        BOOL mv = FALSE;
        for (int i = 0; i < 15; ++i) {   // 目标被占用(看图/杀软扫描)时重试 ~3 秒
            mv = MoveFileExW(partPath_.c_str(), finalPath_.c_str(),
                             MOVEFILE_REPLACE_EXISTING);
            if (mv) break;
            Sleep(200);
        }
        CloseSession();
        int secs = (int)((GetTickCount64() - startTick_.load()) / 1000);
        if (mv) {
            HistAppend(L"done", histId_.load(), url_, finalPath_,
                       totalBytes_.load(), secs, L"");
            SetState(ST_DONE);
            Log(L"下载完成: " + finalPath_ + L" (" + FmtBytes(totalBytes_.load()) + L")");
        } else {
            // 保留 .part 数据 (已完整), 目标文件被外部程序长期占用时用户可手动改名
            HistAppend(L"fail", histId_.load(), url_, partPath_,
                       totalBytes_.load(), secs, L"重命名失败, 数据已保留为 .part");
            SetState(ST_FAILED);
            Log(L"重命名临时文件失败(数据已完整保留): " + partPath_);
        }
    }

    // -------------------- 测速/调度线程 --------------------
    void ControllerEntry() {
        using clock = std::chrono::steady_clock;
        uint64_t last = doneBytes_.load();
        auto lastT = clock::now();
        double baseSpeed = 0.0;       // 上次扩容前的速度基准
        bool   justGrew = false;      // 扩容后尚未评估增益
        int    rampLeft = 0;          // 扩容后剩余的爬坡周期 (跳过, 让新连接提速)
        bool   firstProbe = true;     // 起步立即试探一次
        bool   saturatedLogged = false;
        int    holdTicks = 0;

        for (;;) {
            Sleep(200);
            JobState s = St();
            if (cancel_ || s == ST_DONE || s == ST_FAILED || s == ST_CANCELED ||
                s == ST_IDLE)
                break;

            // 尾部均衡: 队列已空但还有在跑块时, 请求持有者把块一分为二,
            // 让等待中的线程有活干, 避免末尾退化成单连接慢慢爬
            if (!paused_ && inflight_.load() > 0) {
                std::lock_guard<std::mutex> lk(qm_);
                if (pending_.empty()) splitReq_ = 1;
            }

            auto now = clock::now();
            double dt = std::chrono::duration<double>(now - lastT).count();
            if (dt * 1000.0 < (double)std::max(500, intervalMs_.load())) continue;

            uint64_t cur = doneBytes_.load();
            double speed = (cur - last) / dt;
            last = cur;
            lastT = now;
            curSpeed_ = speed;

            if (paused_) continue;

            // 增益探测扩线程: 加倍线程后比较总速度, 提升明显说明还有余量, 继续加;
            // 提升微弱说明带宽已饱和, 维持并周期性重新试探
            if (!adaptive_.load() || s != ST_RUNNING) continue;

            int t = threadTarget_.load();
            int mx = ClampInt(maxThreads_, 1, 256);
            size_t pending = 0;
            {
                std::lock_guard<std::mutex> lk(qm_);
                pending = pending_.size();
            }
            if (t >= mx || pending <= (size_t)t) continue;

            double gainTh = gainThPct_.load();
            if (justGrew) {
                if (rampLeft > 0) { rampLeft--; }   // 新连接爬坡中, 暂不评估
                else {
                    justGrew = false;
                    double gain = baseSpeed > 1.0 ? (speed - baseSpeed) / baseSpeed * 100.0 : 100.0;
                    if (gain >= gainTh) {
                        int nt = std::min(mx, t * 2);
                        Log(L"增益 " + TrimDouble(gain) + L"% >= " + TrimDouble(gainTh) +
                            L"%, 仍有余量, 线程 " + std::to_wstring(t) + L" → " +
                            std::to_wstring(nt) + L" (" + FmtSpeed(speed) + L")");
                        SpawnWorkers(nt - t);
                        threadTarget_ = nt;
                        baseSpeed = speed;
                        justGrew = true;
                        rampLeft = 1;
                        saturatedLogged = false;
                    } else {
                        holdTicks = 0;
                        if (!saturatedLogged) {
                            saturatedLogged = true;
                            Log(L"增益 " + TrimDouble(gain) + L"% < " + TrimDouble(gainTh) +
                                L"%, 带宽趋于饱和, 维持 " + std::to_wstring(t) + L" 线程 (" +
                                FmtSpeed(speed) + L")");
                        }
                    }
                }
            } else if (firstProbe) {
                firstProbe = false;
                int nt = std::min(mx, t * 2);
                Log(L"起步试探: 线程 " + std::to_wstring(t) + L" → " + std::to_wstring(nt));
                SpawnWorkers(nt - t);
                threadTarget_ = nt;
                baseSpeed = speed;
                justGrew = true;
                rampLeft = 1;
            } else if (++holdTicks >= 10) {
                // 饱和后每 10 个测速周期重新试探一次, 捕捉带宽恢复
                holdTicks = 0;
                int nt = std::min(mx, t * 2);
                Log(L"重新试探: 线程 " + std::to_wstring(t) + L" → " + std::to_wstring(nt));
                SpawnWorkers(nt - t);
                threadTarget_ = nt;
                baseSpeed = speed;
                justGrew = true;
                rampLeft = 1;
            }
        }
    }
};

// ============================================================================
//  BT 引擎 —— 通过 aria2c (JSON-RPC) 提供种子/磁力下载
// ============================================================================
static std::string ToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s((size_t)std::max(0, n - 1), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}

static std::wstring FromUtf8(const std::string& u) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, NULL, 0);
    std::wstring w((size_t)std::max(0, n - 1), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, &w[0], n);
    return w;
}

static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'"':  o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        default:
            if ((unsigned)c < 32) {
                wchar_t b[8];
                swprintf(b, 8, L"\\u%04x", c);
                o += b;
            } else o += c;
        }
    }
    return o;
}

// 取 JSON 字符串字段 (aria2 返回值均为字符串)
static std::wstring JsonStr(const std::wstring& j, const wchar_t* key) {
    std::wstring pat = L"\"" + std::wstring(key) + L"\":";
    size_t p = j.find(pat);
    if (p == std::wstring::npos) return L"";
    p += pat.size();
    while (p < j.size() && iswspace(j[p])) ++p;
    if (p >= j.size() || j[p] != L'"') return L"";
    ++p;
    std::wstring out;
    while (p < j.size()) {
        wchar_t c = j[p++];
        if (c == L'\\') {
            if (p >= j.size()) break;
            wchar_t e = j[p++];
            switch (e) {
            case L'"':  out += L'"';  break;
            case L'\\': out += L'\\'; break;
            case L'/':  out += L'/';  break;
            case L'n':  out += L'\n'; break;
            case L't':  out += L'\t'; break;
            case L'r':  out += L'\r'; break;
            case L'u':  if (p + 4 <= j.size()) p += 4; break;
            default: break;
            }
        } else if (c == L'"') break;
        else out += c;
    }
    return out;
}

static std::wstring B64Encode(const std::string& in) {
    static const wchar_t* t =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring o;
    size_t i = 0, n = in.size();
    while (i + 2 < n) {
        uint32_t v = ((unsigned char)in[i] << 16) |
                     ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += t[(v >> 6) & 63];  o += t[v & 63];
        i += 3;
    }
    if (i + 1 == n) {
        uint32_t v = (unsigned char)in[i] << 16;
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63]; o += L"==";
    } else if (i + 2 == n) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63];
        o += L'=';
    }
    return o;
}

static bool ReadFileBin(const std::wstring& p, std::string& out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz)) { CloseHandle(f); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = TRUE;
    if (!out.empty())
        ok = ReadFile(f, &out[0], (DWORD)out.size(), &rd, NULL) && rd == out.size();
    CloseHandle(f);
    return ok;
}

class BtManager {
public:
    std::atomic<JobState> state_{ ST_IDLE };
    std::atomic<uint64_t> done_{ 0 }, total_{ 0 }, speed_{ 0 };
    std::atomic<int>      seeds_{ 0 }, conns_{ 0 };
    std::wstring          dir_;
    HWND ui_ = nullptr;

    JobState St() const { return state_.load(std::memory_order_relaxed); }
    bool Busy() const {
        JobState s = St();
        return s == ST_PROBING || s == ST_RUNNING || s == ST_PAUSED;
    }

    void Log(const std::wstring& s) {
        if (ui_) PostMessageW(ui_, WM_APP_LOG, 0, (LPARAM)(new std::wstring(s)));
    }
    void SetState(JobState s) {
        state_.store(s, std::memory_order_relaxed);
        if (ui_) PostMessageW(ui_, WM_APP_STATE, (WPARAM)s, 0);
    }

    void Start(const std::wstring& src, bool magnet, const std::wstring& dir) {
        if (Busy()) return;
        cancel_ = false;
        metaLogged_ = false;
        gid_.clear();
        done_ = 0; total_ = 0; speed_ = 0; seeds_ = 0; conns_ = 0;
        dir_ = dir;
        if (worker_.joinable()) worker_.join();
        SetState(ST_PROBING);
        try { worker_ = std::thread(&BtManager::Entry, this, src, magnet, dir); }
        catch (...) { SetState(ST_FAILED); Log(L"创建线程失败"); }
    }

    void Pause() {
        if (St() != ST_RUNNING) return;
        Rpc(L"aria2.pause", L"[\"token:" + secret_ + L"\",\"" + gid_ + L"\"]");
        SetState(ST_PAUSED);
        Log(L"已暂停");
    }
    void Resume() {
        if (St() != ST_PAUSED) return;
        Rpc(L"aria2.unpause", L"[\"token:" + secret_ + L"\",\"" + gid_ + L"\"]");
        SetState(ST_RUNNING);
        Log(L"继续下载");
    }
    void Cancel() {
        if (!Busy()) return;
        cancel_ = true;
        SetState(ST_CANCELED);
        HistAppend(L"cancel", histId_.load(), histSrc_, L"", 0, 0, L"");
        if (worker_.joinable()) worker_.join();
        KillEngine();
        SetState(ST_IDLE);
        Log(L"已取消 BT 任务");
    }
    void Shutdown() {              // 退出前收尾
        if (worker_.joinable()) { cancel_ = true; worker_.join(); }
        KillEngine();
    }

private:
    std::wstring gid_, secret_ = L"mthd7f3a9c2e5b18";
    HANDLE proc_ = nullptr;
    std::thread worker_;
    std::atomic<bool> cancel_{ false };
    bool metaLogged_ = false;
    std::atomic<long long> histId_{ 0 };
    std::atomic<unsigned long long> startTick_{ 0 };
    std::wstring histSrc_;

    // ---- JSON-RPC 调用 (本地 aria2c, 127.0.0.1:16800) ----
    static std::wstring Rpc(const std::wstring& method, const std::wstring& params) {
        std::wstring body = L"{\"jsonrpc\":\"2.0\",\"id\":\"1\",\"method\":\"" +
            method + L"\",\"params\":" + params + L"}";
        std::string u8 = ToUtf8(body);
        std::wstring resp;
        HINTERNET s = WinHttpOpen(L"MThDownloader/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s) return L"";
        WinHttpSetTimeouts(s, 400, 400, 1500, 1500);
        HINTERNET c = WinHttpConnect(s, L"127.0.0.1", 16800, 0);
        if (c) {
            HINTERNET rq = WinHttpOpenRequest(c, L"POST", L"/jsonrpc", NULL,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (rq) {
                if (WinHttpSendRequest(rq, L"Content-Type: application/json\r\n", (DWORD)-1,
                        (LPVOID)u8.data(), (DWORD)u8.size(), (DWORD)u8.size(), 0) &&
                    WinHttpReceiveResponse(rq, NULL)) {
                    char buf[4096];
                    DWORD rd = 0;
                    std::string acc;
                    while (WinHttpReadData(rq, buf, sizeof(buf), &rd) && rd > 0)
                        acc.append(buf, rd);
                    resp = FromUtf8(acc);
                }
                WinHttpCloseHandle(rq);
            }
            WinHttpCloseHandle(c);
        }
        WinHttpCloseHandle(s);
        return resp;
    }

    void KillEngine() {
        if (proc_) {
            Rpc(L"aria2.shutdown", L"[\"token:" + secret_ + L"\"]");  // 尽力优雅关闭
            if (WaitForSingleObject(proc_, 2000) != WAIT_OBJECT_0)
                TerminateProcess(proc_, 0);
            CloseHandle(proc_);
            proc_ = nullptr;
        }
    }

    void Fail(const std::wstring& m) {
        Log(L"错误: " + m);
        KillEngine();
        SetState(ST_FAILED);
    }

    void Entry(std::wstring src, bool magnet, std::wstring dir) {
        wchar_t ep[MAX_PATH];
        GetModuleFileNameW(NULL, ep, MAX_PATH);
        std::wstring eng(ep);
        eng.resize(eng.find_last_of(L'\\') + 1);
        eng += L"engine\\aria2c.exe";
        if (GetFileAttributesW(eng.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Fail(L"未找到 BT 引擎 engine\\aria2c.exe");
            return;
        }

        std::wstring tok = L"\"token:" + secret_ + L"\"";
        std::wstring cmd = L"\"" + eng + L"\" --enable-rpc --rpc-listen-port=16800" +
            L" --rpc-secret=" + secret_ +
            L" --dir=\"" + dir + L"\"" +
            L" --file-allocation=falloc --seed-time=0 --continue=true" +
            L" --allow-overwrite=true --bt-max-peers=128" +
            L" --bt-request-peer-speed-limit=20M" +
            L" --listen-port=16881-16891 --dht-listen-port=16881" +
            // DHT 引导点: 加速 DHT 路由表建立, 让磁力/无 tracker 种子更快找到节点
            L" --dht-entry-point=router.bittorrent.com:6881" +
            // 公共 tracker 兜底: aria2 只支持 HTTP(S) tracker (udp:// 会被忽略),
            // 此列表为本机实测可达集, 与种子自带 tracker 合并使用
            L" --bt-tracker=" +
            L"http://tracker2.itzmx.com:6961/announce," +
            L"http://tracker.openbittorrent.com/announce," +
            L"https://tracker.lilithraws.org:443/announce" +
            L" --quiet=true";
        std::vector<wchar_t> cmdv(cmd.begin(), cmd.end());
        cmdv.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(NULL, cmdv.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW,
                NULL, NULL, &si, &pi)) {
            Fail(L"无法启动 BT 引擎进程");
            return;
        }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;

        // 等 RPC 就绪 (最多约 5s)
        bool ready = false;
        for (int i = 0; i < 20 && !cancel_; ++i) {
            Sleep(250);
            if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) break;
            std::wstring r = Rpc(L"aria2.getVersion", L"[" + tok + L"]");
            if (r.find(L"version") != std::wstring::npos) { ready = true; break; }
        }
        if (cancel_) return;
        if (!ready) {
            Fail(L"BT 引擎启动失败 (RPC 无响应, 端口可能被占用)");
            return;
        }
        Log(L"BT 引擎已就绪 (aria2c, DHT/PEX/uTP)");

        std::wstring resp;
        if (magnet) {
            resp = Rpc(L"aria2.addUri", L"[" + tok + L",[\"" + JsonEscape(src) + L"\"]]");
            Log(L"磁力链接已提交, 正在获取元数据...");
        } else {
            std::string bin;
            if (!ReadFileBin(src, bin)) { Fail(L"无法读取种子文件: " + src); return; }
            resp = Rpc(L"aria2.addTorrent", L"[" + tok + L",\"" + B64Encode(bin) + L"\"]");
            Log(L"种子已提交: " + src);
        }
        gid_ = JsonStr(resp, L"gid");
        if (gid_.empty()) gid_ = JsonStr(resp, L"result");   // addUri/addTorrent 直接返回 gid 字符串
        if (gid_.empty()) {
            std::wstring em = JsonStr(resp, L"message");
            Fail(L"添加任务失败: " + (em.empty() ? L"RPC 错误" : em));
            return;
        }
        SetState(ST_RUNNING);
        histId_ = ++g_histSeq;
        startTick_ = GetTickCount64();
        histSrc_ = src;
        HistAppend(L"start", histId_.load(), src, dir, 0, 0, L"");

        // 轮询进度 (UI 定时器从原子变量取值刷新)
        while (!cancel_) {
            Sleep(500);
            std::wstring r = Rpc(L"aria2.tellStatus",
                L"[" + tok + L",\"" + gid_ + L"\"]");
            if (r.empty()) {
                if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {
                    Fail(L"BT 引擎意外退出");
                    return;
                }
                continue;
            }
            auto u64 = [&](const wchar_t* k) {
                uint64_t v = 0;
                ParseUintStr(JsonStr(r, k), v);
                return v;
            };
            done_   = u64(L"completedLength");
            total_  = u64(L"totalLength");
            speed_  = u64(L"downloadSpeed");
            seeds_  = (int)u64(L"numSeeds");
            conns_  = (int)u64(L"connections");
            if (!metaLogged_ && total_ > 0) {
                metaLogged_ = true;
                Log(L"元数据就绪, 总大小 " + FmtBytes(total_));
            }
            std::wstring stt = JsonStr(r, L"status");
            if (stt == L"complete") {
                HistAppend(L"done", histId_.load(), histSrc_, dir,
                           total_.load(),
                           (int)((GetTickCount64() - startTick_.load()) / 1000), L"");
                SetState(ST_DONE);
                Log(L"BT 下载完成: " + dir);
                KillEngine();          // seed-time=0 时引擎通常已自退, 这里兜底
                return;
            }
            if (stt == L"error") {
                HistAppend(L"fail", histId_.load(), histSrc_, dir,
                           done_.load(), 0,
                           L"aria2 errorCode " + JsonStr(r, L"errorCode"));
                SetState(ST_FAILED);
                Log(L"BT 下载失败 (errorCode " + JsonStr(r, L"errorCode") + L")");
                KillEngine();
                return;
            }
            if (stt == L"removed") return;
        }
    }
};

static BtManager g_bt;

// ---------------- 下载历史 (exe 旁 MThDownloader.history.jsonl) ----------------
// 每行一条 JSON: {"id","event"(start/done/fail/cancel),"url","file","size","secs","err","time"}
// 浏览器扩展的下载面板通过本地桥 list 动作读取
static void HistAppend(const std::wstring& event, long long id,
                       const std::wstring& url, const std::wstring& file,
                       uint64_t size, int secs, const std::wstring& err) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring path(p);
    path.resize(path.find_last_of(L'\\') + 1);
    path += L"MThDownloader.history.jsonl";

    std::wstring line = L"{\"id\":" + std::to_wstring(id) +
        L",\"event\":\"" + event +
        L"\",\"url\":\"" + JsonEscape(url) +
        L"\",\"file\":\"" + JsonEscape(file) +
        L"\",\"size\":" + std::to_wstring(size) +
        L",\"secs\":" + std::to_wstring(secs) +
        L",\"time\":" + std::to_wstring((long long)time(nullptr));
    if (!err.empty()) line += L",\"err\":\"" + JsonEscape(err) + L"\"";
    line += L"}\n";

    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string u8 = ToUtf8(line);
    DWORD w = 0;
    WriteFile(f, u8.data(), (DWORD)u8.size(), &w, NULL);
    CloseHandle(f);
}

// ============================================================================
//  GUI
// ============================================================================
static HFONT g_font = nullptr;

static std::wstring GetDlgText(HWND h, int id) {
    HWND c = GetDlgItem(h, id);
    if (!c) return L"";
    int n = GetWindowTextLengthW(c);
    std::wstring s((size_t)std::max(0, n), L'\0');
    if (n > 0) GetWindowTextW(c, &s[0], n + 1);
    return Trim(s);
}

static void SetLog(HWND h, const std::wstring& s) {
    HWND lb = GetDlgItem(h, IDC_LOG);
    int cnt = ListBox_GetCount(lb);
    if (cnt >= 300) ListBox_DeleteString(lb, 0);
    ListBox_AddString(lb, s.c_str());
    ListBox_SetTopIndex(lb, ListBox_GetCount(lb) - 1);
}

static void ApplyStateUI(HWND h, JobState s) {
    auto& d = Downloader::Inst();
    if (g_mode == 1) s = g_bt.St();
    EnableWindow(GetDlgItem(h, IDC_START),
        s == ST_IDLE || s == ST_DONE || s == ST_FAILED);
    EnableWindow(GetDlgItem(h, IDC_CANCEL),
        s == ST_PROBING || s == ST_RUNNING || s == ST_PAUSED);
    SetDlgItemTextW(h, IDC_PAUSE, s == ST_PAUSED ? L"继续" : L"暂停");
    // HTTP 单流模式无法暂停; BT 引擎随时可暂停
    bool canPause = (s == ST_RUNNING || s == ST_PAUSED) && (g_mode == 1 || d.ranged());
    EnableWindow(GetDlgItem(h, IDC_PAUSE), canPause);
}

// 从设置输入框同步参数 (下载中也可实时调整)
static void SyncSettings(HWND h) {
    auto& d = Downloader::Inst();
    auto getInt = [&](int id, int lo, int hi) -> int {
        std::wstring s = GetDlgText(h, id);
        wchar_t* e = nullptr;
        long v = wcstol(s.c_str(), &e, 10);
        if (e == s.c_str()) return -1;
        return (int)std::min(std::max(v, (long)lo), (long)hi);
    };
    auto getDbl = [&](int id, double lo, double hi) -> double {
        std::wstring s = GetDlgText(h, id);
        wchar_t* e = nullptr;
        double v = wcstod(s.c_str(), &e);
        if (e == s.c_str()) return -1;
        return std::min(std::max(v, lo), hi);
    };
    int v;
    if ((v = getInt(IDC_MAXTH, 1, 256)) > 0) d.maxThreads_ = v;
    if ((v = getInt(IDC_INITTH, 1, 64)) > 0) d.initThreads_ = v;
    double f;
    if ((f = getDbl(IDC_SPEEDTH, 1, 100)) > 0) d.gainThPct_ = f;
    if ((f = getDbl(IDC_SIZETH, 0, 1024 * 1024)) >= 0) d.smallFileMB_ = f;
    if ((v = getInt(IDC_INTERVAL, 500, 10000)) > 0) d.intervalMs_ = v;
    if ((v = getInt(IDC_SPLITMIN, 64, 8192)) > 0) d.splitMinKB_ = v;
}

// ---------------- 设置持久化 (exe 旁 MThDownloader.ini, 便携式) ----------------
static std::wstring IniPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s(p);
    size_t b = s.find_last_of(L"\\/");
    return s.substr(0, b + 1) + L"MThDownloader.ini";
}

static void SaveSettings(HWND h) {
    const std::wstring f = IniPath();
    auto set = [&](const wchar_t* k, int id) {
        WritePrivateProfileStringW(L"Settings", k, GetDlgText(h, id).c_str(),
                                   f.c_str());
    };
    set(L"MaxThreads", IDC_MAXTH);
    set(L"InitThreads", IDC_INITTH);
    set(L"GainPct", IDC_SPEEDTH);
    set(L"SmallMB", IDC_SIZETH);
    set(L"IntervalMs", IDC_INTERVAL);
    set(L"SplitKB", IDC_SPLITMIN);
    set(L"SaveDir", IDC_PATH);
    set(L"BtDir", IDC_BT_PATH);
}

static void LoadSettings(HWND h) {
    const std::wstring f = IniPath();
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t buf[512];
    auto get = [&](const wchar_t* k, int id, const wchar_t* def) {
        GetPrivateProfileStringW(L"Settings", k, def, buf, 512, f.c_str());
        if (buf[0]) SetDlgItemTextW(h, id, buf);
    };
    get(L"MaxThreads", IDC_MAXTH, L"128");
    get(L"InitThreads", IDC_INITTH, L"8");
    get(L"GainPct", IDC_SPEEDTH, L"10");
    get(L"SmallMB", IDC_SIZETH, L"8");
    get(L"IntervalMs", IDC_INTERVAL, L"2000");
    get(L"SplitKB", IDC_SPLITMIN, L"512");
    get(L"SaveDir", IDC_PATH, DefaultDownloadDir().c_str());
    get(L"BtDir", IDC_BT_PATH, L"");
}

static void UpdateProgressUI(HWND h) {
    auto& d = Downloader::Inst();
    JobState s;
    uint64_t done, total;
    double speed;
    if (g_mode == 1) {
        s = g_bt.St();
        done = g_bt.done_.load();
        total = g_bt.total_.load();
        speed = (double)g_bt.speed_.load();
    } else {
        s = d.St();
        done = d.doneBytes_.load();
        total = d.totalBytes_.load();
        speed = d.curSpeed_.load();
    }

    HWND pb = GetDlgItem(h, IDC_PROGRESS);
    bool marquee = (s == ST_RUNNING && total == 0);   // 元数据未知 → 走马灯
    static bool curMarq = false;
    if (marquee != curMarq) {
        curMarq = marquee;
        LONG_PTR sty = GetWindowLongPtrW(pb, GWL_STYLE);
        SetWindowLongPtrW(pb, GWL_STYLE, marquee ? (sty | PBS_MARQUEE) : (sty & ~PBS_MARQUEE));
        SendMessageW(pb, PBM_SETMARQUEE, marquee ? 1 : 0, 30);
    }
    if (total > 0)
        SendMessageW(pb, PBM_SETPOS, (WPARAM)(done * 1000 / total), 0);
    else if (s != ST_RUNNING)
        SendMessageW(pb, PBM_SETPOS, 0, 0);

    std::wstring st;
    if (g_mode == 1) {
        switch (s) {
        case ST_PROBING: st = L"正在启动 BT 引擎..."; break;
        case ST_RUNNING: {
            if (total > 0) {
                uint64_t remain = total > done ? total - done : 0;
                std::wstring eta = speed > 512 ? FmtDur(remain / (uint64_t)speed) : L"--";
                wchar_t b[512];
                swprintf(b, 512, L"%ls / %ls (%.1f%%)   %ls   种子 %d / 节点 %d   剩余 %ls",
                    FmtBytes(done).c_str(), FmtBytes(total).c_str(),
                    done * 100.0 / total, SpeedBoth(speed).c_str(),
                    g_bt.seeds_.load(), g_bt.conns_.load(), eta.c_str());
                st = b;
            } else {
                wchar_t b[256];
                swprintf(b, 256, L"获取元数据中...   已连接节点 %d   %ls",
                    g_bt.conns_.load(), SpeedBoth(speed).c_str());
                st = b;
            }
            break;
        }
        case ST_PAUSED:
            st = L"已暂停 " + FmtBytes(done) + (total ? L" / " + FmtBytes(total) : L"");
            break;
        case ST_DONE:   st = L"BT 下载完成: " + g_bt.dir_; break;
        case ST_FAILED: st = L"BT 下载失败 (详见日志)"; break;
        case ST_CANCELED: st = L"已取消"; break;
        default: st = L"就绪";
        }
    } else {
        switch (s) {
        case ST_PROBING: st = L"正在获取文件信息..."; break;
        case ST_RUNNING: {
            if (total > 0) {
                uint64_t remain = total > done ? total - done : 0;
                std::wstring eta = speed > 512 ? FmtDur(remain / (uint64_t)speed) : L"--";
                wchar_t b[512];
                swprintf(b, 512, L"%ls / %ls (%.1f%%)   %ls   线程 %d / %d   剩余 %ls",
                    FmtBytes(done).c_str(), FmtBytes(total).c_str(),
                    done * 100.0 / total, SpeedBoth(speed).c_str(),
                    d.activeWorkers_.load(), d.threadTarget_.load(), eta.c_str());
                st = b;
            } else {
                wchar_t b[256];
                swprintf(b, 256, L"下载中 %ls   %ls   线程 %d (大小未知)",
                    FmtBytes(done).c_str(), SpeedBoth(speed).c_str(),
                    d.activeWorkers_.load());
                st = b;
            }
            break;
        }
        case ST_PAUSED: {
            std::wstring pct = total > 0
                ? L"(" + std::to_wstring(done * 100 / total) + L"%)  " : L"";
            st = L"已暂停 " + pct + FmtBytes(done) +
                (total ? L" / " + FmtBytes(total) : L"");
            break;
        }
        case ST_DONE:   st = L"下载完成: " + d.finalPath_; break;
        case ST_FAILED: st = L"下载失败 (详见日志)"; break;
        case ST_CANCELED: st = L"已取消"; break;
        default: st = L"就绪";
        }
    }
    SetDlgItemTextW(h, IDC_STATUS, st.c_str());
}

static std::wstring DefaultDownloadDir() {
    wchar_t up[MAX_PATH] = L"";
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring d = up;
        d += L"\\Downloads\\";
        if (GetFileAttributesW(d.c_str()) != INVALID_FILE_ATTRIBUTES) return d;
    }
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring exe = buf;
    return exe.substr(0, exe.find_last_of(L'\\') + 1);
}

static void OnBrowse(HWND h) {
    BROWSEINFOW bi{};
    bi.hwndOwner = h;
    bi.lpszTitle = L"选择保存目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, p))
            SetDlgItemTextW(h, IDC_PATH, p);
        CoTaskMemFree(pidl);
    }
}

static void OnStart(HWND h) {
    if (g_bt.Busy()) {
        MessageBoxW(h, L"BT 任务进行中, 请先等待完成或取消", L"提示", MB_ICONINFORMATION);
        return;
    }
    SyncSettings(h);
    auto& d = Downloader::Inst();
    std::wstring url = GetDlgText(h, IDC_URL);
    if (url.empty()) { MessageBoxW(h, L"请输入下载地址", L"提示", MB_ICONINFORMATION); return; }
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) {
        MessageBoxW(h, L"地址需以 http:// 或 https:// 开头", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::wstring dir = GetDlgText(h, IDC_PATH);
    if (dir.empty()) {
        dir = DefaultDownloadDir();
        SetDlgItemTextW(h, IDC_PATH, dir.c_str());
    }
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    // 文件名由下载器探测后强制自动填写 (Content-Disposition / URL), 用户只选目录
    g_mode = 0;
    d.Start(url, dir);
}

// ---- BT 页 ----
static void OnBrowseTorrent(HWND h) {
    wchar_t file[MAX_PATH * 2] = L"";
    lstrcpynW(file, GetDlgText(h, IDC_BT_SRC).c_str(), MAX_PATH * 2);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = h;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrFilter = L"BT 种子 (*.torrent)\0*.torrent\0所有文件 (*.*)\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        SetDlgItemTextW(h, IDC_BT_SRC, file);
}

static void OnBrowseDir(HWND h) {
    BROWSEINFOW bi{};
    bi.hwndOwner = h;
    bi.lpszTitle = L"选择 BT 下载目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, p))
            SetDlgItemTextW(h, IDC_BT_PATH, p);
        CoTaskMemFree(pidl);
    }
}

static void OnBtStart(HWND h) {
    auto& d = Downloader::Inst();
    if (g_bt.Busy()) return;
    if (d.St() == ST_PROBING || d.St() == ST_RUNNING || d.St() == ST_PAUSED) {
        MessageBoxW(h, L"HTTP 任务进行中, 请先等待完成或取消", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::wstring src = GetDlgText(h, IDC_BT_SRC);
    if (src.empty()) {
        MessageBoxW(h, L"请输入种子文件路径或磁力链接", L"提示", MB_ICONINFORMATION);
        return;
    }
    bool magnet = (src.rfind(L"magnet:", 0) == 0);
    if (!magnet && GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(h, L"找不到种子文件, 请检查路径", L"提示", MB_ICONINFORMATION);
        return;
    }
    std::wstring dir = GetDlgText(h, IDC_BT_PATH);
    if (dir.empty()) {
        dir = DefaultDownloadDir();
        SetDlgItemTextW(h, IDC_BT_PATH, dir.c_str());
    }
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    CreateDirectoryW(dir.c_str(), NULL);   // 不存在则创建
    g_mode = 1;
    g_bt.Start(src, magnet, dir);
}

static void SwitchTab(HWND h) {
    bool http = TabCtrl_GetCurSel(GetDlgItem(h, IDC_TAB)) == 0;
    int v1 = http ? SW_SHOW : SW_HIDE;
    int v2 = http ? SW_HIDE : SW_SHOW;
    ShowWindow(GetDlgItem(h, IDC_LURL), v1);
    ShowWindow(GetDlgItem(h, IDC_URL), v1);
    ShowWindow(GetDlgItem(h, IDC_LPATH), v1);
    ShowWindow(GetDlgItem(h, IDC_PATH), v1);
    ShowWindow(GetDlgItem(h, IDC_BROWSE), v1);
    ShowWindow(GetDlgItem(h, IDC_ADAPTGRP), v1);
    ShowWindow(GetDlgItem(h, IDC_LAD1), v1);
    ShowWindow(GetDlgItem(h, IDC_LAD2), v1);
    ShowWindow(GetDlgItem(h, IDC_LAD3), v1);
    ShowWindow(GetDlgItem(h, IDC_LAD4), v1);
    ShowWindow(GetDlgItem(h, IDC_LAD5), v1);
    ShowWindow(GetDlgItem(h, IDC_LAD6), v1);
    ShowWindow(GetDlgItem(h, IDC_MAXTH), v1);
    ShowWindow(GetDlgItem(h, IDC_INITTH), v1);
    ShowWindow(GetDlgItem(h, IDC_SPEEDTH), v1);
    ShowWindow(GetDlgItem(h, IDC_SIZETH), v1);
    ShowWindow(GetDlgItem(h, IDC_INTERVAL), v1);
    ShowWindow(GetDlgItem(h, IDC_SPLITMIN), v1);
    ShowWindow(GetDlgItem(h, IDC_LSRC), v2);
    ShowWindow(GetDlgItem(h, IDC_BT_SRC), v2);
    ShowWindow(GetDlgItem(h, IDC_BT_BROWSE), v2);
    ShowWindow(GetDlgItem(h, IDC_LBTDIR), v2);
    ShowWindow(GetDlgItem(h, IDC_BT_PATH), v2);
    ShowWindow(GetDlgItem(h, IDC_BT_DIRBROWSE), v2);
}

// 命令行/外部转发 URL 的自动填入并立即开始下载 (浏览器扩展桥接入口)
static void AutoStartUrl(HWND h, const std::wstring& url, const std::wstring& dir) {
    if (url.empty()) return;
    if (url.rfind(L"magnet:", 0) == 0) {
        SendMessageW(GetDlgItem(h, IDC_TAB), TCM_SETCURSEL, 1, 0);
        SwitchTab(h);
        SetDlgItemTextW(h, IDC_BT_SRC, url.c_str());
        if (!dir.empty()) SetDlgItemTextW(h, IDC_BT_PATH, dir.c_str());
        OnBtStart(h);
    } else if (url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0) {
        SendMessageW(GetDlgItem(h, IDC_TAB), TCM_SETCURSEL, 0, 0);
        SwitchTab(h);
        SetDlgItemTextW(h, IDC_URL, url.c_str());
        if (!dir.empty()) SetDlgItemTextW(h, IDC_PATH, dir.c_str());
        OnStart(h);
    } else {
        Downloader::Inst().Log(L"不支持的地址协议 (仅 http/https/magnet): " + url);
    }
}

static HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* txt, DWORD style,
               int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto& d = Downloader::Inst();
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

        HWND tab = Mk(h, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            12, 12, 616, 148, IDC_TAB);
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"HTTP 直链";
        TabCtrl_InsertItem(tab, 0, &ti);
        ti.pszText = (LPWSTR)L"BT 种子";
        TabCtrl_InsertItem(tab, 1, &ti);

        // --- HTTP 页 ---
        Mk(h, L"STATIC", L"地址:", 0, 24, 52, 48, 20, IDC_LURL);
        Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 78, 50, 526, 24, IDC_URL, WS_EX_CLIENTEDGE);
        Mk(h, L"STATIC", L"保存目录:", 0, 24, 86, 68, 20, IDC_LPATH);
        Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 78, 84, 440, 24, IDC_PATH, WS_EX_CLIENTEDGE);
        Mk(h, L"BUTTON", L"浏览...", BS_PUSHBUTTON | WS_TABSTOP, 526, 83, 78, 26, IDC_BROWSE);

        // --- BT 页 ---
        Mk(h, L"STATIC", L"种子/磁力:", 0, 24, 52, 68, 20, IDC_LSRC);
        Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 96, 50, 422, 24, IDC_BT_SRC, WS_EX_CLIENTEDGE);
        Mk(h, L"BUTTON", L"选种子...", BS_PUSHBUTTON | WS_TABSTOP, 526, 49, 78, 26, IDC_BT_BROWSE);
        Mk(h, L"STATIC", L"保存目录:", 0, 24, 86, 48, 20, IDC_LBTDIR);
        Mk(h, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 96, 84, 422, 24, IDC_BT_PATH, WS_EX_CLIENTEDGE);
        Mk(h, L"BUTTON", L"选目录...", BS_PUSHBUTTON | WS_TABSTOP, 526, 83, 78, 26, IDC_BT_DIRBROWSE);

        Mk(h, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 12, 172, 616, 22, IDC_PROGRESS);
        SendMessageW(GetDlgItem(h, IDC_PROGRESS), PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        Mk(h, L"STATIC", L"就绪", 0, 12, 202, 616, 20, IDC_STATUS);

        Mk(h, L"BUTTON", L"开始下载", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, 230, 100, 30, IDC_START);
        Mk(h, L"BUTTON", L"暂停", BS_PUSHBUTTON | WS_TABSTOP, 122, 230, 100, 30, IDC_PAUSE);
        Mk(h, L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 232, 230, 100, 30, IDC_CANCEL);

        Mk(h, L"BUTTON", L"HTTP 自适应设置 (下载中可实时调整)",
            BS_GROUPBOX, 12, 270, 616, 84, IDC_ADAPTGRP);
        struct { const wchar_t* label; int id; int lid; const wchar_t* def; int x; } cols[] = {
            { L"最大线程",      IDC_MAXTH,    IDC_LAD1, L"128",  22 },
            { L"初始/保守线程", IDC_INITTH,   IDC_LAD2, L"8",    123 },
            { L"扩容增益 %",    IDC_SPEEDTH,  IDC_LAD3, L"10",   224 },
            { L"小文件阈值 MB", IDC_SIZETH,   IDC_LAD4, L"8",    325 },
            { L"测速间隔 ms",   IDC_INTERVAL, IDC_LAD5, L"2000", 426 },
            { L"拆分下限 KB",   IDC_SPLITMIN, IDC_LAD6, L"512",  527 },
        };
        for (auto& c : cols) {
            Mk(h, L"STATIC", c.label, 0, c.x, 292, 98, 18, c.lid);
            Mk(h, L"EDIT", c.def, ES_AUTOHSCROLL | WS_TABSTOP, c.x, 314, 92, 24, c.id, WS_EX_CLIENTEDGE);
        }

        Mk(h, L"BUTTON", L"日志", BS_GROUPBOX, 12, 366, 616, 286, 0);
        Mk(h, L"LISTBOX", L"", LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL |
            WS_TABSTOP, 24, 386, 592, 256, IDC_LOG, WS_EX_CLIENTEDGE);

        SetDlgItemTextW(h, IDC_URL, L"https://");
        SetDlgItemTextW(h, IDC_PATH, DefaultDownloadDir().c_str());
        LoadSettings(h);             // 恢复上次关闭时的参数与目录
        SwitchTab(h);
        ApplyStateUI(h, ST_IDLE);
        SetTimer(h, 1, 200, NULL);
        DragAcceptFiles(h, TRUE);    // 允许拖入 .torrent 文件
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) SwitchTab(h);
        return 0;
    }
    case WM_DROPFILES: {
        // 拖入 .torrent 种子 → 自动填入 BT 页并切换过去
        HDROP dr = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(dr, 0, path, MAX_PATH)) {
            std::wstring ext = Lower(path);
            size_t dot = ext.find_last_of(L'.');
            if (dot != std::wstring::npos && ext.substr(dot) == L".torrent") {
                SetDlgItemTextW(h, IDC_BT_SRC, path);
                SendMessageW(GetDlgItem(h, IDC_TAB), TCM_SETCURSEL, 1, 0);
                SwitchTab(h);
                SetLog(h, L"已加载种子: " + std::wstring(path));
            } else {
                SetLog(h, L"仅支持拖入 .torrent 种子文件");
            }
        }
        DragFinish(dr);
        return 0;
    }
    case WM_TIMER:
        SyncSettings(h);          // 阈值实时生效
        UpdateProgressUI(h);
        return 0;
    case WM_COPYDATA: {
        // 单实例转发: 第二个进程启动时把 URL 送到这里
        COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lp;
        if (cds && cds->dwData == WM_COPYDATA_URL && cds->lpData &&
            cds->cbData >= sizeof(wchar_t)) {
            std::wstring payload((const wchar_t*)cds->lpData,
                                 cds->cbData / sizeof(wchar_t));
            size_t nl = payload.find(L'\n');
            std::wstring url = payload.substr(0, nl);
            std::wstring dir = (nl == std::wstring::npos) ? L"" : payload.substr(nl + 1);
            AutoStartUrl(h, url, dir);
            ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
            return TRUE;
        }
        return FALSE;
    }
    case WM_APP_AUTOSTART:
        AutoStartUrl(h, g_cmdUrl, g_cmdDir);
        return 0;
    case WM_APP_LOG: {
        auto* s = (std::wstring*)lp;
        SetLog(h, *s);
        delete s;
        return 0;
    }
    case WM_APP_STATE:
        ApplyStateUI(h, (JobState)wp);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BROWSE) OnBrowse(h);
        else if (id == IDC_BT_BROWSE) OnBrowseTorrent(h);
        else if (id == IDC_BT_DIRBROWSE) OnBrowseDir(h);
        else if (id == IDC_START) {
            if (TabCtrl_GetCurSel(GetDlgItem(h, IDC_TAB)) == 1) OnBtStart(h);
            else OnStart(h);
        }
        else if (id == IDC_PAUSE) {
            if (g_bt.Busy()) {
                if (g_bt.St() == ST_RUNNING) g_bt.Pause();
                else if (g_bt.St() == ST_PAUSED) g_bt.Resume();
            } else {
                if (d.St() == ST_RUNNING) d.Pause();
                else if (d.St() == ST_PAUSED) d.Resume();
            }
        }
        else if (id == IDC_CANCEL) {
            if (g_bt.Busy()) g_bt.Cancel();
            else d.Cancel();
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);      // 立即隐藏, 避免清理期间界面卡住的观感
        SaveSettings(h);             // 保存参数, 下次启动恢复
        d.Shutdown();
        g_bt.Shutdown();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    // 命令行: MThDownloader.exe "<url>" [保存目录]
    // 浏览器扩展桥接入口 —— 自动填入并立即开始下载
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc >= 2) g_cmdUrl = argv[1];
        if (argc >= 3) g_cmdDir = argv[2];
        LocalFree(argv);
    }

    // 单实例: 已有窗口则转发 URL 给它并退出,
    // 浏览器批量转发下载时不会拉起多实例 (避免 BT 引擎 RPC 端口冲突)
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\MThDownloader_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!g_cmdUrl.empty()) {
            HWND prev = FindWindowW(L"MThDownloader", NULL);
            if (prev) {
                std::wstring payload = g_cmdUrl + L"\n" + g_cmdDir;
                COPYDATASTRUCT cds{ WM_COPYDATA_URL,
                    (DWORD)((payload.size() + 1) * sizeof(wchar_t)),
                    (PVOID)payload.c_str() };
                SendMessageW(prev, WM_COPYDATA, NULL, (LPARAM)&cds);
                SetForegroundWindow(prev);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MThDownloader";
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 48, 48, LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   16, 16, LR_DEFAULTSIZE);
    RegisterClassExW(&wc);

    // 客户区 640x662
    RECT rc{ 0, 0, 640, 662 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    HWND h = CreateWindowExW(0, L"MThDownloader", L"多线程下载器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, hInst, NULL);
    Downloader::Inst().ui_ = h;
    g_bt.ui_ = h;
    ShowWindow(h, nCmdShow);
    UpdateWindow(h);
    if (!g_cmdUrl.empty())
        PostMessageW(h, WM_APP_AUTOSTART, 0, 0);   // 命令行 URL 自动开始下载

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (IsDialogMessageW(h, &m)) continue;   // 支持 Tab 切换焦点
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
