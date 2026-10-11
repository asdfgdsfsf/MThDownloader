/*
 * MThDownloader 浏览器下载桥接宿主 (Chrome/Edge Native Messaging, 独立 exe, 零依赖)
 *
 * 协议 (Chrome Native Messaging 标准):
 *   stdin  : uint32(小端, 长度) + UTF-8 JSON
 *   stdout : uint32(小端, 长度) + UTF-8 JSON
 *
 * 收到 {"action":"ping"}                        -> 健康检查
 * 收到 {"action":"download","url":...,...}      -> 按 bridge_config.json 启动 MThDownloader.exe
 *
 * 配置 (同目录 bridge_config.json, 可用环境变量 MTH_BRIDGE_CONFIG 覆盖):
 *   executable      : exe 绝对路径
 *   args_template   : 参数模板数组, 占位符 {url} {filename} {referrer} {mime}
 *   allowed_schemes : 协议白名单 (http/https/ftp/magnet)
 *   max_url_length  : URL 最大长度
 *
 * 安全: 可执行文件路径只来自本地配置, 不信任浏览器侧数据; URL 协议白名单校验。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MSG (64 * 1024 * 1024)
#define MAX_URL (64 * 1024)

/* ---------------- 最小 JSON 解析 (对象/数组/字符串/数字/布尔/null) ---------------- */
typedef enum { JSTR, JNUM, JBOOL, JNULL, JARR, JOBJ } JType;
typedef struct JVal {
    JType t;
    char* s;                 /* JSTR */
    double num;              /* JNUM */
    int b;                   /* JBOOL */
    struct JVal** items;     /* JARR 元素 / JOBJ 值 */
    char** keys;             /* JOBJ 键 */
    int n;
} JVal;

static void jfree(JVal* v) {
    if (!v) return;
    free(v->s);
    for (int i = 0; i < v->n; ++i) {
        jfree(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

static const char* jws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

/* 把 UTF-16 码点转 UTF-8 追加到动态字符串 */
typedef struct { char* p; size_t len, cap; } Buf;
static void buf_put(Buf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->p = (char*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_put_u16(Buf* b, unsigned cp) {
    char t[4];
    int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    buf_put(b, t, (size_t)n);
}

static int hex4(const char* p) {
    int v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* 解析 JSON 字符串 (假定 *pp 指向开引号), 返回 malloc 的 UTF-8 */
static char* jstr(const char** pp) {
    const char* p = *pp;
    if (*p != '"') return NULL;
    ++p;
    Buf b = { (char*)malloc(64), 0, 64 };
    b.p[0] = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            switch (*p) {
            case '"': buf_put(&b, "\"", 1); break;
            case '\\': buf_put(&b, "\\", 1); break;
            case '/': buf_put(&b, "/", 1); break;
            case 'n': buf_put(&b, "\n", 1); break;
            case 't': buf_put(&b, "\t", 1); break;
            case 'r': buf_put(&b, "\r", 1); break;
            case 'b': buf_put(&b, "\b", 1); break;
            case 'f': buf_put(&b, "\f", 1); break;
            case 'u': {
                int cp = hex4(p + 1);
                if (cp < 0) { free(b.p); return NULL; }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    int lo = hex4(p + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                buf_put_u16(&b, (unsigned)cp);
                break;
            }
            default: free(b.p); return NULL;
            }
            ++p;
        } else {
            buf_put(&b, p, 1);
            ++p;
        }
    }
    if (*p != '"') { free(b.p); return NULL; }
    *pp = p + 1;
    return b.p;
}

static JVal* jparse(const char** pp, int depth);

static JVal* jnew(JType t) {
    JVal* v = (JVal*)calloc(1, sizeof(JVal));
    v->t = t;
    return v;
}

static JVal* jparse(const char** pp, int depth) {
    if (depth > 32) return NULL;
    const char* p = jws(*pp);
    JVal* v;
    if (*p == '"') {
        v = jnew(JSTR);
        v->s = jstr(&p);
        if (!v->s) { jfree(v); return NULL; }
    } else if (*p == '{') {
        v = jnew(JOBJ);
        ++p;
        p = jws(p);
        if (*p == '}') { *pp = p + 1; return v; }
        for (;;) {
            p = jws(p);
            char* k = jstr(&p);
            if (!k) { jfree(v); return NULL; }
            p = jws(p);
            if (*p != ':') { free(k); jfree(v); return NULL; }
            ++p;   /* 跳过冒号 */
            JVal* item = jparse(&p, depth + 1);
            if (!item) { free(k); jfree(v); return NULL; }
            v->keys = (char**)realloc(v->keys, sizeof(char*) * (v->n + 1));
            v->items = (JVal**)realloc(v->items, sizeof(JVal*) * (v->n + 1));
            v->keys[v->n] = k;
            v->items[v->n] = item;
            ++v->n;
            p = jws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { *pp = p + 1; return v; }
            jfree(v); return NULL;
        }
    } else if (*p == '[') {
        v = jnew(JARR);
        ++p;
        p = jws(p);
        if (*p == ']') { *pp = p + 1; return v; }
        for (;;) {
            JVal* item = jparse(&p, depth + 1);
            if (!item) { jfree(v); return NULL; }
            v->items = (JVal**)realloc(v->items, sizeof(JVal*) * (v->n + 1));
            v->items[v->n++] = item;
            p = jws(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { *pp = p + 1; return v; }
            jfree(v); return NULL;
        }
    } else if (!strncmp(p, "true", 4)) {
        v = jnew(JBOOL); v->b = 1; p += 4;
    } else if (!strncmp(p, "false", 5)) {
        v = jnew(JBOOL); v->b = 0; p += 5;
    } else if (!strncmp(p, "null", 4)) {
        v = jnew(JNULL); p += 4;
    } else {
        char* e;
        double d = strtod(p, &e);
        if (e == p) return NULL;
        v = jnew(JNUM); v->num = d; p = e;
    }
    *pp = p;
    return v;
}

static JVal* jget(JVal* v, const char* key) {
    if (!v || v->t != JOBJ) return NULL;
    for (int i = 0; i < v->n; ++i)
        if (!strcmp(v->keys[i], key)) return v->items[i];
    return NULL;
}

/* ---------------- JSON 序列化 (响应输出) ---------------- */
static void jesc(Buf* b, const char* s) {
    buf_put(b, "\"", 1);
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        switch (*p) {
        case '"': buf_put(b, "\\\"", 2); break;
        case '\\': buf_put(b, "\\\\", 2); break;
        case '\n': buf_put(b, "\\n", 2); break;
        case '\r': buf_put(b, "\\r", 2); break;
        case '\t': buf_put(b, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                char t[8];
                sprintf(t, "\\u%04x", *p);
                buf_put(b, t, 6);
            } else {
                buf_put(b, (const char*)p, 1);
            }
        }
    }
    buf_put(b, "\"", 1);
}

/* ---------------- 工具 ---------------- */
static int read_exact(unsigned char* p, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t r = fread(p + got, 1, n - got, stdin);
        if (r == 0) return 0;
        got += r;
    }
    return 1;
}

static void send_json(const char* s) {
    size_t n = strlen(s);
    fwrite(&(unsigned int){ (unsigned int)n }, 1, 4, stdout);   /* 小端机器直接写 */
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

/* UTF-8 -> UTF-16 */
static wchar_t* u8w(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*)malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* list2cmdline: Windows 参数引号规则 */
static void qarg(Buf* b, const char* a) {
    if (!strpbrk(a, " \t\"")) { buf_put(b, a, strlen(a)); return; }
    buf_put(b, "\"", 1);
    int bs = 0;
    for (const char* p = a; ; ++p) {
        if (*p == '\\') { ++bs; continue; }
        int emit = bs;
        if (*p == '"') emit = bs * 2 + 1;
        for (int i = 0; i < emit; ++i) buf_put(b, "\\", 1);
        bs = 0;
        if (!*p) break;
        if (*p == '"') buf_put(b, "\\\"", 2);
        else buf_put(b, p, 1);
    }
    buf_put(b, "\"", 1);
}

/* ---------------- 配置 ---------------- */
typedef struct {
    char* executable;
    char** args; int nargs;
    char** schemes; int nschemes;
    int max_url;
} Config;

static void cfg_default(Config* c, const char* hostdir) {
    c->executable = (char*)malloc(strlen(hostdir) + 32);
    sprintf(c->executable, "%sMThDownloader.exe", hostdir);
    c->args = (char**)malloc(sizeof(char*));
    c->args[0] = strdup("{url}");
    c->nargs = 1;
    static char* def_sc[] = { (char*)"http", (char*)"https", (char*)"ftp" };
    c->schemes = def_sc;
    c->nschemes = 3;
    c->max_url = 8192;
}

static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char* s = (char*)malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); fclose(f); return NULL; }
    s[n] = 0;
    fclose(f);
    return s;
}

/* 读文件尾部 (最多 maxb 字节), 供 history.jsonl 增量读取 */
static char* slurp_tail(const char* path, size_t maxb) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if ((size_t)n > maxb) fseek(f, -(long)maxb, SEEK_END);
    else fseek(f, 0, SEEK_SET);
    size_t cap = ((size_t)n > maxb) ? maxb : (size_t)n;
    char* s = (char*)malloc(cap + 1);
    if (!s) { fclose(f); return NULL; }
    size_t r = fread(s, 1, cap, f);
    fclose(f);
    s[r] = 0;
    return s;
}

static void cfg_path(char* out, size_t n) {
    const char* env = getenv("MTH_BRIDGE_CONFIG");
    if (env) { snprintf(out, n, "%s", env); return; }
    char exe[MAX_PATH * 2];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char* s = strrchr(exe, '\\');
    if (s) *(s + 1) = 0;
    snprintf(out, n, "%sbridge_config.json", exe);
}

/* 把配置完整写回 bridge_config.json (浏览器侧 set_config 修改路径后调用) */
static int cfg_save(Config* c) {
    char path[MAX_PATH * 4];
    cfg_path(path, sizeof(path));
    Buf b = { (char*)malloc(4096), 0, 4096 };
    b.p[0] = 0;
    buf_put(&b, "{\n  \"executable\": ", strlen("{\n  \"executable\": "));
    jesc(&b, c->executable);
    buf_put(&b, ",\n  \"args_template\": [", strlen(",\n  \"args_template\": ["));
    for (int i = 0; i < c->nargs; ++i) {
        if (i) buf_put(&b, ", ", 2);
        jesc(&b, c->args[i]);
    }
    buf_put(&b, "],\n  \"allowed_schemes\": [", strlen("],\n  \"allowed_schemes\": ["));
    for (int i = 0; i < c->nschemes; ++i) {
        if (i) buf_put(&b, ", ", 2);
        jesc(&b, c->schemes[i]);
    }
    char t[64];
    sprintf(t, "],\n  \"max_url_length\": %d\n}\n", c->max_url);
    buf_put(&b, t, strlen(t));
    FILE* f = fopen(path, "wb");
    if (!f) { free(b.p); return 0; }
    size_t w = fwrite(b.p, 1, b.len, f);
    fclose(f);
    free(b.p);
    return w == b.len;
}

static void cfg_load(Config* c) {
    char dir[MAX_PATH * 2];
    {
        char exe[MAX_PATH * 2];
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        char* s = strrchr(exe, '\\');
        if (s) *(s + 1) = 0;
        strcpy(dir, exe);
    }
    char path[MAX_PATH * 4];
    cfg_path(path, sizeof(path));

    cfg_default(c, dir);

    char* raw = slurp(path);
    if (!raw) return;
    const char* p = raw;
    JVal* root = jparse(&p, 0);
    if (!root) { free(raw); return; }

    JVal* v;
    if ((v = jget(root, "executable")) && v->t == JSTR) {
        free(c->executable);
        c->executable = strdup(v->s);
    }
    if ((v = jget(root, "args_template")) && v->t == JARR) {
        for (int i = 0; i < c->nargs; ++i) free(c->args[i]);
        c->nargs = 0;
        c->args = (char**)malloc(sizeof(char*) * (v->n ? v->n : 1));
        for (int i = 0; i < v->n; ++i)
            if (v->items[i]->t == JSTR) c->args[c->nargs++] = strdup(v->items[i]->s);
    }
    if ((v = jget(root, "allowed_schemes")) && v->t == JARR && v->n > 0) {
        c->schemes = NULL;
        c->nschemes = v->n;
        c->schemes = (char**)malloc(sizeof(char*) * v->n);
        for (int i = 0; i < v->n; ++i)
            if (v->items[i]->t == JSTR) c->schemes[i] = strdup(v->items[i]->s);
    }
    if ((v = jget(root, "max_url_length")) && v->t == JNUM)
        c->max_url = (int)v->num;

    jfree(root);
    free(raw);
}

/* ---------------- 业务 ---------------- */
static void str_lower(char* s) { for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s += 32; }

static const char* url_scheme(const char* url) {
    const char* c = strstr(url, "://");
    static char scheme[32];
    if (!c || (size_t)(c - url) >= sizeof(scheme)) return "";
    memcpy(scheme, url, (size_t)(c - url));
    scheme[c - url] = 0;
    str_lower(scheme);
    return scheme;
}

int main(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    Config cfg;
    cfg_load(&cfg);

    for (;;) {
        unsigned char hdr[4];
        if (!read_exact(hdr, 4)) break;
        unsigned long len = (unsigned long)hdr[0] | ((unsigned long)hdr[1] << 8) |
                            ((unsigned long)hdr[2] << 16) | ((unsigned long)hdr[3] << 24);
        if (len == 0 || len > MAX_MSG) break;
        char* raw = (char*)malloc(len + 1);
        if (!read_exact((unsigned char*)raw, len)) { free(raw); break; }
        raw[len] = 0;

        const char* p = raw;
        JVal* msg = jparse(&p, 0);
        if (!msg) { free(raw); send_json("{\"ok\":false,\"error\":\"invalid message\"}"); continue; }

        JVal* act = jget(msg, "action");
        if (act && act->t == JSTR && !strcmp(act->s, "ping")) {
            DWORD fa = GetFileAttributesA(cfg.executable);
            int ready = (fa != INVALID_FILE_ATTRIBUTES) && !(fa & FILE_ATTRIBUTE_DIRECTORY);
            char* ex8 = cfg.executable;   /* 已是 UTF-8 (配置文件按 UTF-8 读) */
            Buf b = { (char*)malloc(1024), 0, 1024 };
            b.p[0] = 0;
            buf_put(&b, "{\"ok\":true,\"action\":\"ping\",\"executable\":", strlen("{\"ok\":true,\"action\":\"ping\",\"executable\":"));
            jesc(&b, ex8);
            char t[64];
            sprintf(t, ",\"executable_ready\":%s}", ready ? "true" : "false");
            buf_put(&b, t, strlen(t));
            send_json(b.p);
            free(b.p);
        } else if (act && act->t == JSTR && !strcmp(act->s, "download")) {
            JVal* ju = jget(msg, "url");
            const char* url = (ju && ju->t == JSTR) ? ju->s : "";
            const char* err = NULL;
            if (!*url) err = "empty url";
            else if ((int)strlen(url) > cfg.max_url) err = "url too long";
            else {
                const char* sc = url_scheme(url);
                int ok = 0;
                for (int i = 0; i < cfg.nschemes && !ok; ++i)
                    if (!strcmp(sc, cfg.schemes[i])) ok = 1;
                if (!ok) err = "scheme not allowed";
            }
            DWORD fa = GetFileAttributesA(cfg.executable);
            int exe_ok = (fa != INVALID_FILE_ATTRIBUTES) && !(fa & FILE_ATTRIBUTE_DIRECTORY);
            if (!err && !exe_ok) err = "executable not found";

            if (err) {
                /* 组完整错误文本后再序列化, 保证 JSON 合法 */
                char errtext[MAX_URL + 128];
                snprintf(errtext, sizeof(errtext), "%s", err);
                if (!strcmp(err, "scheme not allowed")) {
                    sprintf(errtext, "scheme not allowed: %s", url_scheme(url));
                } else if (!strcmp(err, "executable not found")) {
                    sprintf(errtext, "executable not found: %s", cfg.executable);
                }
                Buf b = { (char*)malloc(MAX_URL + 256), 0, MAX_URL + 256 };
                b.p[0] = 0;
                buf_put(&b, "{\"ok\":false,\"error\":", strlen("{\"ok\":false,\"error\":"));
                jesc(&b, errtext);
                buf_put(&b, "}", 1);
                send_json(b.p);
                free(b.p);
            } else {
                /* 组命令行: executable + 模板替换 */
                const char* fn = ""; JVal* t;
                if ((t = jget(msg, "filename")) && t->t == JSTR) fn = t->s;
                const char* ref = "";
                if ((t = jget(msg, "referrer")) && t->t == JSTR) ref = t->s;
                const char* mime = "";
                if ((t = jget(msg, "mime")) && t->t == JSTR) mime = t->s;

                Buf cmd = { (char*)malloc(4096), 0, 4096 };   /* 引号后命令行 */
                Buf rep = { (char*)malloc(4096), 0, 4096 };   /* 响应 command 数组 */
                cmd.p[0] = rep.p[0] = 0;
                qarg(&cmd, cfg.executable);
                buf_put(&rep, "[", 1); jesc(&rep, cfg.executable);

                for (int i = 0; i < cfg.nargs; ++i) {
                    /* 占位符替换 (动态长度, 防溢出) */
                    const char* keys[4] = { "{url}", "{filename}", "{referrer}", "{mime}" };
                    char* vals[4] = { (char*)url, (char*)fn, (char*)ref, (char*)mime };
                    size_t cap = strlen(cfg.args[i]) + strlen(url) +
                                 strlen(fn) + strlen(ref) + strlen(mime) + 16;
                    char* tmp = (char*)malloc(cap);
                    tmp[0] = 0;
                    const char* src = cfg.args[i];
                    for (;;) {
                        int hit = -1;
                        size_t hitpos = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char* q = strstr(src, keys[k]);
                            if (q && (hit < 0 || (size_t)(q - src) < hitpos)) {
                                hit = k;
                                hitpos = (size_t)(q - src);
                            }
                        }
                        if (hit < 0) { strcat(tmp, src); break; }
                        strncat(tmp, src, hitpos);
                        strcat(tmp, vals[hit]);
                        src += hitpos + strlen(keys[hit]);
                    }
                    buf_put(&cmd, " ", 1);
                    qarg(&cmd, tmp);
                    buf_put(&rep, ",", 1); jesc(&rep, tmp);
                    free(tmp);
                }
                buf_put(&rep, "]", 1);

                /* 启动 (CREATE_NO_WINDOW, cwd=exe 目录) */
                wchar_t* cmdw = u8w(cmd.p);
                wchar_t exew[MAX_PATH * 2];
                MultiByteToWideChar(CP_UTF8, 0, cfg.executable, -1, exew, MAX_PATH * 2);
                wchar_t cwd[MAX_PATH * 2];
                wcscpy(cwd, exew);
                wchar_t* sl = wcsrchr(cwd, L'\\');
                if (sl) *(sl + 1) = 0; else wcscpy(cwd, L".");
                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi;
                BOOL ok = CreateProcessW(NULL, cmdw, NULL, NULL, FALSE,
                                         CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
                if (ok) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
                free(cmdw);

                Buf out = { (char*)malloc(4096), 0, 4096 };
                out.p[0] = 0;
                if (ok) buf_put(&out, "{\"ok\":true,\"action\":\"download\",\"command\":", strlen("{\"ok\":true,\"action\":\"download\",\"command\":"));
                else { buf_put(&out, "{\"ok\":false,\"error\":\"launch failed\",", strlen("{\"ok\":false,\"error\":\"launch failed\",")); }
                buf_put(&out, rep.p, rep.len);
                buf_put(&out, "}", 1);
                send_json(out.p);
                free(out.p);
                free(cmd.p);
                free(rep.p);
            }
        } else if (act && act->t == JSTR && !strcmp(act->s, "set_config")) {
            /* 浏览器扩展选项页设置 MThDownloader.exe 路径, 校验后写回配置文件 */
            JVal* je = jget(msg, "executable");
            const char* exe = (je && je->t == JSTR) ? je->s : "";
            const char* err = NULL;
            size_t elen = strlen(exe);
            if (!elen) err = "empty executable";
            else if (elen >= MAX_PATH * 2) err = "executable path too long";
            else {
                int absolute = (elen >= 3 && exe[1] == ':') || exe[0] == '\\' || exe[0] == '/';
                if (!absolute) err = "executable must be an absolute path";
                else if (elen < 4 || _stricmp(exe + elen - 4, ".exe") != 0)
                    err = "executable must be a .exe file";
            }
            if (!err) {
                DWORD fa = GetFileAttributesA(exe);
                if (fa == INVALID_FILE_ATTRIBUTES || (fa & FILE_ATTRIBUTE_DIRECTORY))
                    err = "executable not found";
            }
            if (err) {
                char errtext[MAX_PATH * 2 + 64];
                if (!strcmp(err, "executable not found"))
                    sprintf(errtext, "executable not found: %s", exe);
                else
                    snprintf(errtext, sizeof(errtext), "%s", err);
                Buf b = { (char*)malloc(MAX_PATH * 2 + 128), 0, MAX_PATH * 2 + 128 };
                b.p[0] = 0;
                buf_put(&b, "{\"ok\":false,\"error\":", strlen("{\"ok\":false,\"error\":"));
                jesc(&b, errtext);
                buf_put(&b, "}", 1);
                send_json(b.p);
                free(b.p);
            } else {
                free(cfg.executable);
                cfg.executable = strdup(exe);
                int saved = cfg_save(&cfg);
                Buf b = { (char*)malloc(1024), 0, 1024 };
                b.p[0] = 0;
                if (saved) {
                    buf_put(&b, "{\"ok\":true,\"action\":\"set_config\",\"executable\":", strlen("{\"ok\":true,\"action\":\"set_config\",\"executable\":"));
                    jesc(&b, cfg.executable);
                    buf_put(&b, "}", 1);
                } else {
                    buf_put(&b, "{\"ok\":false,\"error\":\"failed to write bridge_config.json\"}", strlen("{\"ok\":false,\"error\":\"failed to write bridge_config.json\"}"));
                }
                send_json(b.p);
                free(b.p);
            }
        } else if (act && act->t == JSTR && !strcmp(act->s, "list")) {
            /* 读 MThDownloader.history.jsonl 尾部 (文件在配置的 MThDownloader.exe 目录),
               每行一个 JSON 对象直接拼数组返回 */
            char hpath[MAX_PATH * 4];
            snprintf(hpath, sizeof(hpath), "%s", cfg.executable);
            char* sl2 = strrchr(hpath, '\\');
            if (sl2) *(sl2 + 1) = 0;
            strcat(hpath, "MThDownloader.history.jsonl");
            char* raw = slurp_tail(hpath, 256 * 1024);
            Buf b = { (char*)malloc(300 * 1024), 0, 300 * 1024 };
            b.p[0] = 0;
            buf_put(&b, "{\"ok\":true,\"items\":[", strlen("{\"ok\":true,\"items\":["));
            int count = 0;
            if (raw) {
                for (char* ln = raw; *ln && count < 256; ) {
                    char* nl = strchr(ln, '\n');
                    char* end = nl ? nl : ln + strlen(ln);
                    while (ln < end && (*ln == ' ' || *ln == '\r' || *ln == '\t')) ++ln;
                    while (end > ln && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) --end;
                    if (end > ln && *ln == '{') {
                        if (count) buf_put(&b, ",", 1);
                        buf_put(&b, ln, (size_t)(end - ln));
                        ++count;
                    }
                    if (!nl) break;
                    ln = nl + 1;
                }
                free(raw);
            }
            buf_put(&b, "]}", 2);
            send_json(b.p);
            free(b.p);
        } else {
            char actname[64] = "?";
            if (act && act->t == JSTR) { snprintf(actname, sizeof(actname), "%s", act->s); }
            Buf b = { (char*)malloc(128), 0, 128 };
            b.p[0] = 0;
            buf_put(&b, "{\"ok\":false,\"error\":\"unsupported action: ", strlen("{\"ok\":false,\"error\":\"unsupported action: "));
            buf_put(&b, actname, strlen(actname));
            buf_put(&b, "\"}", 2);
            send_json(b.p);
            free(b.p);
        }
        jfree(msg);
        free(raw);
    }
    return 0;
}
