/*
 * QuickRun - 快速启动器（集成 Everything SDK）
 * ------------------------------------------------------------
 * 编译（TDM-GCC，64 位）：
 *     windres app.rc -o app_res.o
 *     gcc QuickRun.c app_res.o -o QuickRun.exe -O2 -mwindows -s -lshell32 -ldwmapi
 *
 * 需要将 Everything64.dll 放在 QuickRun.exe 同目录。
 * Everything 必须在后台运行，否则文件结果自动降级为无。
 * ------------------------------------------------------------
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- 控件 ID ---- */
#define ID_EDIT      1001
#define ID_MENU      1002
#define ID_LIST      1003
#define ID_DATA      1004
#define ID_HELP      1005
#define ID_RESETPOS  1007
#define ID_OPEN_FOLDER  1008

#define WM_QR_ACTIVATE (WM_APP + 1)

/* ---- 尺寸限制 ---- */
#define MAX_INPUT   512
#define MAX_ARGS    32
#define MAX_ENTRIES 2048
#define MAX_RESULTS 9

/* Everything SDK 常量 */
#define EV_SORT_RUN_COUNT_DESC  20

/* =========================================================
 *  Everything SDK 动态加载
 * ========================================================= */

typedef void  (WINAPI *PFN_SetSearchW)(LPCWSTR);
typedef BOOL  (WINAPI *PFN_QueryW)(BOOL);
typedef DWORD (WINAPI *PFN_GetNumResults)(void);
typedef DWORD (WINAPI *PFN_GetResultFullPathNameW)(DWORD, LPWSTR, DWORD);
typedef void  (WINAPI *PFN_SetSort)(DWORD);
typedef void  (WINAPI *PFN_SetMax)(DWORD);

static HMODULE g_hEv = NULL;
static PFN_SetSearchW              g_evSetSearch   = NULL;
static PFN_QueryW                  g_evQuery       = NULL;
static PFN_GetNumResults           g_evGetNum      = NULL;
static PFN_GetResultFullPathNameW  g_evGetPath     = NULL;
static PFN_SetSort                 g_evSetSort     = NULL;
static PFN_SetMax                  g_evSetMax      = NULL;

static void EvLoad(void)
{
    g_hEv = LoadLibraryW(L"Everything64.dll");
    if (!g_hEv) return;

    g_evSetSearch = (PFN_SetSearchW)GetProcAddress(g_hEv, "Everything_SetSearchW");
    g_evQuery     = (PFN_QueryW)GetProcAddress(g_hEv, "Everything_QueryW");
    g_evGetNum    = (PFN_GetNumResults)GetProcAddress(g_hEv, "Everything_GetNumResults");
    g_evGetPath   = (PFN_GetResultFullPathNameW)GetProcAddress(g_hEv, "Everything_GetResultFullPathNameW");
    g_evSetSort   = (PFN_SetSort)GetProcAddress(g_hEv, "Everything_SetSort");
    g_evSetMax    = (PFN_SetMax)GetProcAddress(g_hEv, "Everything_SetMax");
}

static void EvUnload(void)
{
    if (g_hEv) { FreeLibrary(g_hEv); g_hEv = NULL; }
}

/* =========================================================
 *  全局状态
 * ========================================================= */

static WCHAR g_input[MAX_INPUT];
static WCHAR g_prefill[MAX_INPUT] = { 0 };   /* --input= 预填文本 */
static WCHAR g_args[MAX_ARGS][MAX_INPUT];
static int   g_argCount = 0;
static BOOL  g_noFallback = FALSE;
static BOOL  g_executed = FALSE;
static WCHAR g_dataFile[MAX_PATH * 2];

typedef struct {
    WCHAR key[MAX_INPUT];
    WCHAR cmd[MAX_INPUT];
    WCHAR desc[MAX_INPUT];
    BOOL  useCmd;
    BOOL  hidden;
} Entry;

static Entry g_entries[MAX_ENTRIES];
static int   g_entryCount = 0;

/* 匹配结果 */
static WCHAR g_matches[MAX_RESULTS][MAX_INPUT];       
static WCHAR g_matchFullPaths[MAX_RESULTS][MAX_INPUT];
static WCHAR g_matchCmds[MAX_RESULTS][MAX_INPUT];
static WCHAR g_matchDescs[MAX_RESULTS][MAX_INPUT];
static BOOL  g_matchUseCmds[MAX_RESULTS];
static int   g_matchCount = 0;
static int   g_selected = -1;

static WNDPROC g_oldEditProc = NULL;
static BOOL    g_listVisible = FALSE;
static int     g_initW = 0, g_initH = 0, g_expandedH = 0;

/* 字体和 Edit 坐标 */
static HFONT   g_hFontUI = NULL;
static HFONT   g_hFontEdit = NULL;
static RECT    g_rcEdit = { 0 }; 

/* 状态栏文字 */
static WCHAR   g_statusText[MAX_INPUT] = { 0 };

/* 防止弹窗时失焦退出 */
static BOOL    g_inModal = FALSE; 

/* 前置声明 */
static void ForceForeground(HWND hWnd);
static void ExecuteInput(HWND hDlg, BOOL forceFallback);
static void UpdateSearch(HWND hDlg);
static void ShowList(HWND hDlg, BOOL show);
static void RunCommandEx(const WCHAR *cmd, const WCHAR *desc, BOOL useCmd);
static void UpdateStatusBar(HWND hDlg);
static void SaveWindowPos(HWND hDlg);
static BOOL LoadWindowPos(int *x, int *y);

/* =========================================================
 *  帮助文本
 * ========================================================= */

static const WCHAR *kHelpText =
    L"QuickRun 关键字命令查询器（集成 Everything）\n"
    L"\n"
    L"用法：\n"
    L"  QuickRun.exe                      弹窗（用 exe 目录下 QuickRun.txt）\n"
    L"  QuickRun.exe 关键字 [参数...]     直接执行\n"
    L"  QuickRun.exe --data=路径 关键字   用指定的数据文件\n"
    L"  QuickRun.exe --no-fallback        禁用回退\n"
    L"  QuickRun.exe --input=文本         弹窗并预填输入框（光标在开头）\n"
    L"  QuickRun.exe -h                   显示本帮助\n"
    L"\n"
    L"操作：\n"
    L"  输入关键字       列表实时过滤（命令结果优先，文件结果在后）\n"
    L"  ↑ / ↓            在列表里选\n"
    L"  Alt+I / Alt+K    同上\n"
    L"  Alt+1 ~ Alt+9    直接执行对应编号的条目\n"
    L"  Ctrl+D           打开选中文件所在目录（仅文件结果）\n"
    L"  Enter            执行（选中项 / 精确匹配 / 列表第一项 / 回退）\n"
    L"  Alt+Enter        无视列表与匹配，原样执行输入文本（类似 Win+R）\n"
    L"  Esc              关闭\n"
    L"  F1               显示本帮助\n"
    L"  F2               打开数据文件\n"
    L"  F4               重置窗口位置（居中）\n"
    L"\n"
    L"文件结果：\n"
    L"  输入不含空格时，除命令结果外，还会追加 Everything 中\n"
    L"  扩展名为 .exe 的文件结果，按运行次数排序，最多 10 条。\n"
    L"  选中文件结果回车 → ShellExecute 打开。\n"
    L"  需要 Everything 在后台运行，且 Everything64.dll 在 exe 目录。\n"
    L"\n"
    L"输入含空格时：\n"
    L"  第一个词当作关键字精确查表，后面当作参数。\n"
    L"  参数占位符：{1} {2}… {0}（关键字本身） {*}（全部）\n"
    L"\n"
    L"命令字段前缀：\n"
    L"  !cmd!  在可见 cmd 窗口执行（cmd /k）\n"
    L"  !clip! 替换为剪贴板文本\n"
    L"  第一字段可用 | 分隔多个关键字\n"
    L"  第一字段以 @ 开头：隐藏此条\n"
    L"\n"
    L"回退：\n"
    L"  搜索不到也不精确匹配时，直接把输入当系统命令执行\n"
    L"  （类似 Win+R）。加 --no-fallback 可关闭。\n"
    L"\n"
    L"QuickRun.txt 每行：\n"
    L"  关键字[,关键字...][,!cmd!]要执行的命令,描述\n";

/* =========================================================
 *  工具
 * ========================================================= */

static void TrimW(WCHAR *s)
{
    WCHAR *p = s;
    size_t n;

    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));

    n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' '  || s[n - 1] == L'\t' ||
                     s[n - 1] == L'\r' || s[n - 1] == L'\n'))
        s[--n] = 0;
}

static BOOL ContainsIgnoreCase(const WCHAR *hay, const WCHAR *needle)
{
    if (!*needle) return TRUE;
    size_t nl = wcslen(needle);
    for (const WCHAR *p = hay; *p; p++) {
        if (_wcsnicmp(p, needle, nl) == 0) return TRUE;
    }
    return FALSE;
}

static void AppendQuotedArg(WCHAR *dst, size_t cap, size_t *pos, const WCHAR *src)
{
    size_t p = *pos;
    if (p + 2 >= cap) return;
    dst[p++] = L'"';
    while (*src) {
        if (*src == L'"') {
            if (p + 3 >= cap) break;
            dst[p++] = L'\\';
        }
        if (p + 2 >= cap) break;
        dst[p++] = *src++;
    }
    dst[p++] = L'"';
    dst[p] = 0;
    *pos = p;
}

static BOOL GetClipboardText(WCHAR *buf, size_t bufSize)
{
    if (!buf || bufSize == 0) return FALSE;
    buf[0] = 0;
    if (!OpenClipboard(NULL)) return FALSE;

    BOOL ok = FALSE;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        WCHAR *p = (WCHAR *)GlobalLock(hData);
        if (p) {
            wcsncpy(buf, p, bufSize - 1);
            buf[bufSize - 1] = 0;
            GlobalUnlock(hData);
            ok = TRUE;
        }
    }
    CloseClipboard();
    return ok;
}

static void SplitArgs(const WCHAR *s, int *outCount)
{
    int n = 0;
    const WCHAR *p = s;

    while (*p && n < MAX_ARGS) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;

        const WCHAR *start;
        size_t len;

        if (*p == L'"') {
            p++;
            start = p;
            while (*p && *p != L'"') p++;
            len = (size_t)(p - start);
            if (*p == L'"') p++;
        } else {
            start = p;
            while (*p && *p != L' ' && *p != L'\t') p++;
            len = (size_t)(p - start);
        }

        if (len >= MAX_INPUT) len = MAX_INPUT - 1;
        wcsncpy(g_args[n], start, len);
        g_args[n][len] = 0;
        n++;
    }
    *outCount = n;
}

static void SubstituteArgs(const WCHAR *src, WCHAR *dst, size_t dstSize)
{
    size_t di = 0;
    const WCHAR *p = src;

    while (*p && di + 1 < dstSize) {
        if (wcsncmp(p, L"!clip!", 6) == 0) {
            WCHAR clip[MAX_INPUT * 4];
            if (GetClipboardText(clip, MAX_INPUT * 4)) {
                size_t clen = wcslen(clip);
                if (di + clen < dstSize) {
                    memcpy(dst + di, clip, clen * sizeof(WCHAR));
                    di += clen;
                }
            }
            p += 6;
            continue;
        }

        if (*p == L'{') {
            const WCHAR *close = wcschr(p + 1, L'}');
            if (close && close - p >= 2 && close - p <= 4) {
                WCHAR inner[4] = { 0 };
                size_t n = (size_t)(close - p - 1);
                if (n < 4) wcsncpy(inner, p + 1, n);

                if (inner[0] == L'*' && inner[1] == 0) {
                    for (int i = 0; i < g_argCount; i++) {
                        if (i > 0 && di + 1 < dstSize) dst[di++] = L' ';
                        size_t alen = wcslen(g_args[i]);
                        if (di + alen < dstSize) {
                            memcpy(dst + di, g_args[i], alen * sizeof(WCHAR));
                            di += alen;
                        }
                    }
                    p = close + 1;
                    continue;
                }

                BOOL isNum = (n > 0);
                for (size_t k = 0; k < n; k++) {
                    if (inner[k] < L'0' || inner[k] > L'9') {
                        isNum = FALSE; break;
                    }
                }
                if (isNum) {
                    int idx = _wtoi(inner);
                    const WCHAR *val = NULL;
                    if (idx == 0) val = g_input;
                    else if (idx >= 1 && idx <= g_argCount) val = g_args[idx - 1];

                    if (val) {
                        size_t vlen = wcslen(val);
                        if (di + vlen < dstSize) {
                            memcpy(dst + di, val, vlen * sizeof(WCHAR));
                            di += vlen;
                        }
                    }
                    p = close + 1;
                    continue;
                }
            }
        }
        dst[di++] = *p++;
    }
    dst[di] = 0;
}

/* =========================================================
 *  执行命令
 * ========================================================= */

static void RunCommandEx(const WCHAR *cmd, const WCHAR *desc, BOOL useCmd)
{
    WCHAR substituted[4096];
    WCHAR expanded[4096];
    WCHAR line[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    size_t n;
    BOOL ok = FALSE;
    DWORD err = 0;

    SubstituteArgs(cmd, substituted, 4096);

    if (ExpandEnvironmentStringsW(substituted, expanded, 4096) == 0) {
        wcsncpy(expanded, substituted, 4095);
        expanded[4095] = 0;
    }

    if (useCmd) {
        WCHAR cmdLine[4400];
        wsprintfW(cmdLine, L"cmd /k %s", expanded);

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.lpTitle = (LPWSTR)((desc && *desc) ? desc : L"QuickRun");
        ZeroMemory(&pi, sizeof(pi));

        if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
            DWORD e = GetLastError();
            WCHAR msg[2000];
            wsprintfW(msg, L"命令执行失败：\n\n%s\n\n错误码：%lu", expanded, e);
            g_inModal = TRUE;
            MessageBoxW(NULL, msg, L"错误", MB_ICONERROR);
            g_inModal = FALSE;
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }

    n = wcslen(expanded);
    if (n > 4095) n = 4095;
    memcpy(line, expanded, n * sizeof(WCHAR));
    line[n] = 0;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, line, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ok = TRUE;
    } else {
        err = GetLastError();
        HINSTANCE hi = ShellExecuteW(NULL, L"open", expanded,
                                     NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hi > 32) ok = TRUE;
        else                  err = (DWORD)(INT_PTR)hi;
    }

    if (!ok) {
        WCHAR msg[2400];
        wsprintfW(msg,
            L"命令执行失败：\n\n原始：%s\n\n替换参数后：%s\n\n"
            L"展开环境变量后：%s\n\n说明：%s\n\n错误码：%lu",
            cmd, substituted, expanded, desc, err);
        g_inModal = TRUE;
        MessageBoxW(NULL, msg, L"错误", MB_ICONERROR);
        g_inModal = FALSE;
    }
}

/* =========================================================
 *  数据文件加载
 * ========================================================= */

static BOOL LoadDataFile(const WCHAR *file)
{
    FILE  *fp;
    long   sz;
    size_t got;
    char  *raw;
    WCHAR *wbuf;
    int    wlen;
    UINT   cp = CP_UTF8;

    g_entryCount = 0;

    fp = _wfopen(file, L"rb");
    if (!fp) return FALSE;

    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0) { fclose(fp); return FALSE; }
    if (sz > 4 * 1024 * 1024) sz = 4 * 1024 * 1024;

    raw = (char *)malloc((size_t)sz + 1);
    if (!raw) { fclose(fp); return FALSE; }

    got = fread(raw, 1, (size_t)sz, fp);
    fclose(fp);
    raw[got] = 0;

    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               raw, (int)got, NULL, 0);
    if (wlen <= 0) {
        cp   = CP_ACP;
        wlen = MultiByteToWideChar(CP_ACP, 0, raw, (int)got, NULL, 0);
    }
    if (wlen <= 0) { free(raw); return FALSE; }

    wbuf = (WCHAR *)malloc(((size_t)wlen + 1) * sizeof(WCHAR));
    if (!wbuf) { free(raw); return FALSE; }

    MultiByteToWideChar(cp, 0, raw, (int)got, wbuf, wlen);
    wbuf[wlen] = 0;
    free(raw);

    if (wbuf[0] == 0xFEFF)
        memmove(wbuf, wbuf + 1, ((size_t)wlen) * sizeof(WCHAR));

    WCHAR *p = wbuf;
    while (*p && g_entryCount < MAX_ENTRIES) {
        WCHAR *eol  = wcschr(p, L'\n');
        WCHAR *line = p;
        size_t ln;

        if (eol) *eol = 0;

        ln = wcslen(line);
        while (ln > 0 && (line[ln - 1] == L'\r' ||
                          line[ln - 1] == L' '  ||
                          line[ln - 1] == L'\t'))
            line[--ln] = 0;

        WCHAR *s = line;
        while (*s == L' ' || *s == L'\t') s++;

        if (*s && *s != L'#' && *s != L';') {
            WCHAR *c1 = wcschr(s, L',');
            if (c1) {
                WCHAR *f2, *c2, *desc;

                *c1 = 0;
                f2  = c1 + 1;
                c2  = wcschr(f2, L',');

                if (c2) { *c2 = 0; desc = c2 + 1; }
                else    { desc = (WCHAR *)L""; }

                TrimW(s);
                TrimW(f2);
                TrimW(desc);

                BOOL hidden = FALSE;
                if (*s == L'@') { hidden = TRUE; s++; TrimW(s); }

                BOOL useCmd = FALSE;
                if (wcsncmp(f2, L"!cmd!", 5) == 0) {
                    useCmd = TRUE; f2 += 5; TrimW(f2);
                }

                if (*s && *f2) {
                    WCHAR *tok = s;
                    while (*tok && g_entryCount < MAX_ENTRIES) {
                        WCHAR *bar  = wcschr(tok, L'|');
                        WCHAR *next = NULL;
                        if (bar) { *bar = 0; next = bar + 1; }

                        TrimW(tok);
                        if (*tok) {
                            Entry *e = &g_entries[g_entryCount];
                            wcsncpy(e->key,  tok,  MAX_INPUT - 1);
                            wcsncpy(e->cmd,  f2,   MAX_INPUT - 1);
                            wcsncpy(e->desc, desc, MAX_INPUT - 1);
                            e->key[MAX_INPUT - 1]  = 0;
                            e->cmd[MAX_INPUT - 1]  = 0;
                            e->desc[MAX_INPUT - 1] = 0;
                            e->useCmd = useCmd;
                            e->hidden = hidden;
                            g_entryCount++;
                        }
                        if (!next) break;
                        tok = next;
                    }
                }
            }
        }

        if (!eol) break;
        p = eol + 1;
    }

    free(wbuf);
    return TRUE;
}

/* =========================================================
 *  搜索：命令结果优先，Everything 文件结果追加在后
 * ========================================================= */

static void SearchEntries(const WCHAR *input)
{
    g_matchCount = 0;
    if (!input[0]) return;

    size_t inLen = wcslen(input);
    int n = 0;

    /* 完全匹配 */
    for (int i = 0; i < g_entryCount && n < MAX_RESULTS; i++) {
        if (g_entries[i].hidden) continue;
        if (_wcsicmp(g_entries[i].key, input) == 0) {
            wcsncpy(g_matches[n],       g_entries[i].key,  MAX_INPUT - 1);
            wcsncpy(g_matchFullPaths[n],g_entries[i].cmd,  MAX_INPUT - 1);
            wcsncpy(g_matchCmds[n],     g_entries[i].cmd,  MAX_INPUT - 1);
            wcsncpy(g_matchDescs[n],    g_entries[i].desc, MAX_INPUT - 1);
            g_matches[n][MAX_INPUT-1] = 0;
            g_matchFullPaths[n][MAX_INPUT-1] = 0;
            g_matchCmds[n][MAX_INPUT-1] = 0;
            g_matchDescs[n][MAX_INPUT-1] = 0;
            g_matchUseCmds[n] = g_entries[i].useCmd;
            n++;
        }
    }

    /* 前缀匹配 */
    for (int i = 0; i < g_entryCount && n < MAX_RESULTS; i++) {
        if (g_entries[i].hidden) continue;
        if (_wcsicmp(g_entries[i].key, input) == 0) continue;
        if (_wcsnicmp(g_entries[i].key, input, inLen) == 0) {
            wcsncpy(g_matches[n],       g_entries[i].key,  MAX_INPUT - 1);
            wcsncpy(g_matchFullPaths[n],g_entries[i].cmd,  MAX_INPUT - 1);
            wcsncpy(g_matchCmds[n],     g_entries[i].cmd,  MAX_INPUT - 1);
            wcsncpy(g_matchDescs[n],    g_entries[i].desc, MAX_INPUT - 1);
            g_matches[n][MAX_INPUT-1] = 0;
            g_matchFullPaths[n][MAX_INPUT-1] = 0;
            g_matchCmds[n][MAX_INPUT-1] = 0;
            g_matchDescs[n][MAX_INPUT-1] = 0;
            g_matchUseCmds[n] = g_entries[i].useCmd;
            n++;
        }
    }

    /* 子串匹配 */
    for (int i = 0; i < g_entryCount && n < MAX_RESULTS; i++) {
        if (g_entries[i].hidden) continue;
        if (_wcsicmp(g_entries[i].key, input) == 0) continue;
        if (_wcsnicmp(g_entries[i].key, input, inLen) == 0) continue;
        if (ContainsIgnoreCase(g_entries[i].key, input)) {
            wcsncpy(g_matches[n],       g_entries[i].key,  MAX_INPUT - 1);
            wcsncpy(g_matchFullPaths[n],g_entries[i].cmd,  MAX_INPUT - 1);
            wcsncpy(g_matchCmds[n],     g_entries[i].cmd,  MAX_INPUT - 1);
            wcsncpy(g_matchDescs[n],    g_entries[i].desc, MAX_INPUT - 1);
            g_matches[n][MAX_INPUT-1] = 0;
            g_matchFullPaths[n][MAX_INPUT-1] = 0;
            g_matchCmds[n][MAX_INPUT-1] = 0;
            g_matchDescs[n][MAX_INPUT-1] = 0;
            g_matchUseCmds[n] = g_entries[i].useCmd;
            n++;
        }
    }

    /* Everything 文件结果 */
    if (n < MAX_RESULTS && g_evSetSearch && g_evQuery &&
        g_evGetNum && g_evGetPath) {

        WCHAR q[MAX_INPUT + 256];
        wsprintfW(q, L"%s ext:exe;lnk !\"C:\\Windows\\SysWOW64\\\" !\"C:\\Windows\\WinSxS\\\" !\"C:\\Windows\\SystemApps\\\" !\"C:\\Windows\\servicing\\\" !\"C:\\Windows\\Microsoft.NET\\\"", input);

        g_evSetSearch(q);
        if (g_evSetSort) g_evSetSort(EV_SORT_RUN_COUNT_DESC);
        if (g_evSetMax)  g_evSetMax((DWORD)(MAX_RESULTS - n));

        if (g_evQuery(TRUE)) {
            DWORD cnt = g_evGetNum();
            for (DWORD i = 0; i < cnt && n < MAX_RESULTS; i++) {
                WCHAR fullPath[MAX_INPUT];
                g_evGetPath(i, fullPath, MAX_INPUT);
                fullPath[MAX_INPUT - 1] = 0;

                wcsncpy(g_matchFullPaths[n], fullPath, MAX_INPUT - 1);
                g_matchFullPaths[n][MAX_INPUT - 1] = 0;

                WCHAR *fileName = wcsrchr(fullPath, L'\\');
                if (fileName) fileName++;
                else          fileName = fullPath;
                
                wcsncpy(g_matches[n], fileName, MAX_INPUT - 1);
                g_matches[n][MAX_INPUT - 1] = 0;

                g_matchCmds[n][0] = 0;      /* 标记为文件结果 */
                g_matchDescs[n][0] = 0;
                g_matchUseCmds[n] = FALSE;
                n++;
            }
        }
    }

    g_matchCount = n;
}

/* =========================================================
 *  对话框模板
 * ========================================================= */

static union { BYTE b[4096]; DWORD d; } g_tpl;
static BYTE *g_p;

static void PW(WORD v)          { memcpy(g_p, &v, 2); g_p += 2; }
static void PD(DWORD v)         { memcpy(g_p, &v, 4); g_p += 4; }
static void PS(const WCHAR *s)  { size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
                                  memcpy(g_p, s, n); g_p += n; }
static void PA(void)            { while (((ULONG_PTR)g_p) & 3) *g_p++ = 0; }

static void BuildTemplate(void)
{
    g_p = g_tpl.b;

    PD(WS_POPUP | DS_CENTER | DS_SETFONT);
    PD(WS_EX_TOOLWINDOW);
    PW(3);
    PW(0); PW(0);
    PW(206); PW(23); 
    PW(0);
    PW(0);
    PS(L"QuickRun");
    PW(9);
    PS(L"Microsoft YaHei UI");

    /* 编辑框 */
    PA();
    PD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL);
    PD(0); 
    PW(5); PW(4); PW(196); PW(14);
    PW(ID_EDIT);
    PW(0xFFFF); PW(0x0081);
    PS(L"");
    PW(0);

    /* 列表 */
    PA();
    PD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED |
       LBS_NOTIFY | LBS_HASSTRINGS);
    PD(WS_EX_CLIENTEDGE); 
    PW(0); PW(22); PW(206); PW(128); 
    PW(ID_LIST);
    PW(0xFFFF); PW(0x0083);
    PS(L"");
    PW(0);

    /* 隐藏按钮 */
    PA();
    PD(WS_CHILD | BS_DEFPUSHBUTTON);
    PD(0);
    PW(0); PW(0); PW(1); PW(1);
    PW(IDOK);
    PW(0xFFFF); PW(0x0080);
    PS(L"OK");
    PW(0);
}

/* =========================================================
 *  列表显示/隐藏 + 窗口动态高度
 * ========================================================= */

static void ShowList(HWND hDlg, BOOL show)
{
    if (g_listVisible == show) return;
    g_listVisible = show;

    HWND hList = GetDlgItem(hDlg, ID_LIST);

    RECT rc;
    GetWindowRect(hDlg, &rc);
    int newH = show ? g_expandedH : g_initH;

    SetWindowPos(hDlg, NULL, 0, 0,
                 rc.right - rc.left, newH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right - rc.left + 1, newH + 1, 5, 5);
    SetWindowRgn(hDlg, hRgn, TRUE);

    ShowWindow(hList, show ? SW_SHOW : SW_HIDE);

    RedrawWindow(hDlg, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW); 
}

/* =========================================================
 *  更新状态栏文字
 * ========================================================= */

static void UpdateStatusBar(HWND hDlg)
{
    if (g_selected >= 0 && g_selected < g_matchCount) {
        wcsncpy(g_statusText, g_matchFullPaths[g_selected], MAX_INPUT - 1);
    } else {
        wcsncpy(g_statusText, L"↑↓ 选择 | Enter 执行 |  Alt+Enter 原样执行 | F1 帮助", MAX_INPUT - 1);
    }
    g_statusText[MAX_INPUT - 1] = 0;

    InvalidateRect(hDlg, NULL, TRUE);
}

/* =========================================================
 *  输入变化 → 更新搜索/列表
 * ========================================================= */

static void UpdateSearch(HWND hDlg)
{
    WCHAR buf[MAX_INPUT];
    GetDlgItemTextW(hDlg, ID_EDIT, buf, MAX_INPUT);

    HWND hList = GetDlgItem(hDlg, ID_LIST);

    if (!buf[0] || wcschr(buf, L' ') || wcschr(buf, L'\t')) {
        SendMessageW(hList, LB_RESETCONTENT, 0, 0);
        g_matchCount = 0;
        g_selected = -1;          
        ShowList(hDlg, FALSE);
        UpdateStatusBar(hDlg);    
        return;
    }

    TrimW(buf);
    if (!buf[0]) {
        SendMessageW(hList, LB_RESETCONTENT, 0, 0);
        g_matchCount = 0;
        g_selected = -1;
        ShowList(hDlg, FALSE);
        UpdateStatusBar(hDlg);
        return;
    }

    SearchEntries(buf);

    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    for (int i = 0; i < g_matchCount; i++) {
        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"");
        if (idx >= 0)
            SendMessageW(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
    }

    ShowList(hDlg, g_matchCount > 0);

    if (g_matchCount > 0) {
        g_selected = 0;
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
    } else {
        g_selected = -1;
    }

    UpdateStatusBar(hDlg);
}

/* =========================================================
 *  执行输入内容
 * ========================================================= */

static void ExecuteInput(HWND hDlg, BOOL forceFallback)
{
    WCHAR buf[MAX_INPUT];
    GetDlgItemTextW(hDlg, ID_EDIT, buf, MAX_INPUT);
    TrimW(buf);

    if (!buf[0]) return;

    if (!forceFallback) {
        if (g_selected >= 0 && g_selected < g_matchCount) {
            if (g_matchCmds[g_selected][0] == 0) {
                HINSTANCE hi = ShellExecuteW(NULL, L"open",
                                             g_matchFullPaths[g_selected],
                                             NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)hi <= 32) {
                    WCHAR msg[800];
                    wsprintfW(msg, L"打开失败：\n%s\n错误码：%d",
                              g_matchFullPaths[g_selected], (int)(INT_PTR)hi);
                    g_inModal = TRUE;
                    MessageBoxW(hDlg, msg, L"错误", MB_ICONERROR);
                    g_inModal = FALSE;
                }
            } else {
                wcsncpy(g_input, g_matches[g_selected], MAX_INPUT - 1);
                g_input[MAX_INPUT - 1] = 0;
                g_argCount = 0;
                RunCommandEx(g_matchCmds[g_selected], g_matchDescs[g_selected],
                             g_matchUseCmds[g_selected]);
            }
            g_executed = TRUE;
            return;
        }

        WCHAR *sp = wcschr(buf, L' ');
        if (!sp) sp = wcschr(buf, L'\t');

        if (sp) {
            WCHAR key[MAX_INPUT];
            size_t klen = (size_t)(sp - buf);
            if (klen >= MAX_INPUT) klen = MAX_INPUT - 1;
            wcsncpy(key, buf, klen);
            key[klen] = 0;
            TrimW(key);

            g_argCount = 0;
            SplitArgs(sp + 1, &g_argCount);

            BOOL matched = FALSE;
            for (int i = 0; i < g_entryCount; i++) {
                if (_wcsicmp(g_entries[i].key, key) == 0) {
                    wcsncpy(g_input, key, MAX_INPUT - 1);
                    g_input[MAX_INPUT - 1] = 0;
                    RunCommandEx(g_entries[i].cmd, g_entries[i].desc,
                                 g_entries[i].useCmd);
                    g_executed = TRUE;
                    matched = TRUE;
                    break;
                }
            }
            if (matched) return;
        } else {
            for (int i = 0; i < g_entryCount; i++) {
                if (_wcsicmp(g_entries[i].key, buf) == 0) {
                    wcsncpy(g_input, buf, MAX_INPUT - 1);
                    g_input[MAX_INPUT - 1] = 0;
                    g_argCount = 0;
                    RunCommandEx(g_entries[i].cmd, g_entries[i].desc,
                                 g_entries[i].useCmd);
                    g_executed = TRUE;
                    return;
                }
            }

            if (g_matchCount > 0) {
                g_selected = 0;
                if (g_matchCmds[0][0] == 0) {
                    HINSTANCE hi = ShellExecuteW(NULL, L"open",
                                                 g_matchFullPaths[0],
                                                 NULL, NULL, SW_SHOWNORMAL);
                    if ((INT_PTR)hi <= 32) {
                        WCHAR msg[800];
                        wsprintfW(msg, L"打开失败：\n%s\n错误码：%d",
                                  g_matchFullPaths[0], (int)(INT_PTR)hi);
                        g_inModal = TRUE;
                        MessageBoxW(hDlg, msg, L"错误", MB_ICONERROR);
                        g_inModal = FALSE;
                    }
                } else {
                    wcsncpy(g_input, g_matches[0], MAX_INPUT - 1);
                    g_input[MAX_INPUT - 1] = 0;
                    g_argCount = 0;
                    RunCommandEx(g_matchCmds[0], g_matchDescs[0], g_matchUseCmds[0]);
                }
                g_executed = TRUE;
                return;
            }
        }
    }

    if (g_noFallback) {
        WCHAR msg[600];
        wsprintfW(msg, L"没有找到与“%s”匹配的命令。", buf);
        g_inModal = TRUE;
        MessageBoxW(hDlg, msg, L"提示", MB_ICONINFORMATION);
        g_inModal = FALSE;
        return;
    }

    HINSTANCE hi = ShellExecuteW(NULL, L"open", buf,
                                 NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hi <= 32) {
        WCHAR cmdLine[2048];
        wsprintfW(cmdLine, L"cmd /c %s || pause", buf);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                            0, NULL, NULL, &si, &pi)) {
            WCHAR msg[600];
            wsprintfW(msg, L"无法执行：%s\n错误码：%d",
                      buf, (int)(INT_PTR)hi);
            g_inModal = TRUE;
            MessageBoxW(hDlg, msg, L"提示", MB_ICONINFORMATION);
            g_inModal = FALSE;
        } else {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    g_executed = TRUE;
}

/* =========================================================
 *  窗口位置记忆
 * ========================================================= */
static void GetIniPath(WCHAR *path, size_t cap)
{
    WCHAR appData[MAX_PATH];
    ExpandEnvironmentStringsW(L"%APPDATA%", appData, MAX_PATH);
    
    WCHAR dir[MAX_PATH];
    wsprintfW(dir, L"%s\\QuickRun", appData);
    CreateDirectoryW(dir, NULL);
    
    wsprintfW(path, L"%s\\QuickRun\\QuickRun.ini", appData);
}

static void SaveWindowPos(HWND hDlg)
{
    RECT rc;
    GetWindowRect(hDlg, &rc);
    WCHAR buf[32];
    WCHAR iniPath[MAX_PATH];
    
    GetIniPath(iniPath, MAX_PATH);
    
    wsprintfW(buf, L"%d", rc.left);
    WritePrivateProfileStringW(L"Window", L"X", buf, iniPath);
    
    wsprintfW(buf, L"%d", rc.top);
    WritePrivateProfileStringW(L"Window", L"Y", buf, iniPath);
}

static BOOL LoadWindowPos(int *x, int *y)
{
    WCHAR iniPath[MAX_PATH];
    GetIniPath(iniPath, MAX_PATH);

    int px = GetPrivateProfileIntW(L"Window", L"X", -9999, iniPath);
    int py = GetPrivateProfileIntW(L"Window", L"Y", -9999, iniPath);
    
    if (px == -9999 || py == -9999) return FALSE;

    int vX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (px < vX || px > vX + vW - 50 || py < vY || py > vY + vH - 50) {
        return FALSE;
    }

    *x = px;
    *y = py;
    return TRUE;
}

/* =========================================================
 *  强制抢前台
 * ========================================================= */

static void ForceForeground(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);

    HWND hFg = GetForegroundWindow();
    DWORD fgThread = hFg ? GetWindowThreadProcessId(hFg, NULL) : 0;
    DWORD myThread = GetCurrentThreadId();
    BOOL attached = FALSE;

    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);

    if (fgThread && fgThread != myThread) {
        attached = AttachThreadInput(fgThread, myThread, TRUE);
    }
    
    SetForegroundWindow(hWnd);
    BringWindowToTop(hWnd);
    
    HWND hEdit = GetDlgItem(hWnd, ID_EDIT);
    if (hEdit) {
        SetFocus(hEdit);
        SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    }

    if (attached) {
        AttachThreadInput(fgThread, myThread, FALSE);
    }
}

/* =========================================================
 *  编辑框子类化
 * ========================================================= */

static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg,
                                         WPARAM wp, LPARAM lp)
{
    if (msg == WM_GETDLGCODE) {
        LRESULT base = CallWindowProcW(g_oldEditProc, hWnd, msg, wp, lp);
        if (lp) {
            MSG *m = (MSG *)lp;
            BOOL isSpecialKey = FALSE;
            if (m->message == WM_SYSKEYDOWN) {
                WPARAM k = m->wParam;
                if ((k >= L'1' && k <= L'9') ||
                    (k >= VK_NUMPAD1 && k <= VK_NUMPAD9) ||
                    k == L'I' || k == L'K' || k == VK_RETURN)
                    isSpecialKey = TRUE;
            } else if (m->message == WM_SYSCHAR) {
                if ((GetKeyState(VK_MENU) & 0x8000) && m->wParam != L' ')
                    isSpecialKey = TRUE;
            } else if (m->message == WM_KEYDOWN) {
                if ((GetKeyState(VK_CONTROL) & 0x8000) && m->wParam == L'D')
                    isSpecialKey = TRUE;
            }
            if (isSpecialKey)
                base |= DLGC_WANTALLKEYS;
        }
        return base;
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        HWND hList = GetDlgItem(GetParent(hWnd), ID_LIST);
        BOOL alt = (GetKeyState(VK_MENU) & 0x8000) ? TRUE : FALSE;

        if (msg == WM_KEYDOWN) {
            if (wp == VK_F1) {
                PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_HELP, 0), 0);
                return 0;
            }
            if (wp == VK_F2) {
                PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_DATA, 0), 0);
                return 0;
            }
            if (wp == VK_F4) { /* F4 重置位置 */
                PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_RESETPOS, 0), 0);
                return 0;
            }
            /* Ctrl+D 打开选中文件所在目录 */
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == L'D') {
                PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_OPEN_FOLDER, 0), 0);
                return 0;
            }
        }

        if (alt) {
            if (wp == VK_RETURN) {
                PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, 1), 0);
                return 0;
            }

            int n = -1;
            if (wp >= L'1' && wp <= L'9')
                n = (int)(wp - L'1');
            else if (wp >= VK_NUMPAD1 && wp <= VK_NUMPAD9)
                n = (int)(wp - VK_NUMPAD1);

            if (n >= 0 && g_matchCount > 0 && n < g_matchCount) {
                g_selected = n;
                PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
                return 0;
            }

            if (wp == L'I') wp = VK_UP;
            else if (wp == L'K') wp = VK_DOWN;
        }

        if (wp == VK_UP) {
            if (g_matchCount > 0) {
                int newSel;
                if (g_selected <= 0) {
                    newSel = g_matchCount - 1;
                } else {
                    newSel = g_selected - 1;
                }
                
                if (newSel != g_selected) {
                    g_selected = newSel;
                    SendMessageW(hList, LB_SETCURSEL, (WPARAM)g_selected, 0);
                    UpdateStatusBar(GetParent(hWnd));
                }
            }
            return 0;
        }
        
        if (wp == VK_DOWN) {
            if (g_matchCount > 0) {
                int newSel;
                if (g_selected < 0 || g_selected >= g_matchCount - 1) {
                    newSel = 0;
                } else {
                    newSel = g_selected + 1;
                }

                if (newSel != g_selected) {
                    g_selected = newSel;
                    SendMessageW(hList, LB_SETCURSEL, g_selected, 0);
                    UpdateStatusBar(GetParent(hWnd));
                }
            }
            return 0;
        }
    }

    if (msg == WM_SYSCHAR) {
        if (wp >= L'1' && wp <= L'9') {
            int n = (int)(wp - L'1');
            if (g_matchCount > 0 && n < g_matchCount)
                return 0;
        } else if (wp == 0x0D) {
            return 0;
        } else if ((GetKeyState(VK_MENU) & 0x8000) &&
                   g_matchCount > 0 && wp != L' ') {
            return 0;
        }
    }

    return CallWindowProcW(g_oldEditProc, hWnd, msg, wp, lp);
}

/* =========================================================
 *  对话框过程
 * ========================================================= */

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {

    case WM_INITDIALOG: {
        #ifndef DWMWA_WINDOW_CORNER_PREFERENCE
        #define DWMWA_WINDOW_CORNER_PREFERENCE 33
        #endif
        int pref = 2; /* DWMWCP_ROUND */
        DwmSetWindowAttribute(hDlg, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

        HWND hEdit = GetDlgItem(hDlg, ID_EDIT);
        HWND hList = GetDlgItem(hDlg, ID_LIST);
        
        g_hFontUI = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        g_hFontEdit = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
                                  DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

        SendMessageW(hList, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(hEdit, WM_SETFONT, (WPARAM)g_hFontEdit, TRUE);

        GetWindowRect(hEdit, &g_rcEdit);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcEdit, 2);

        {
            HINSTANCE hInst = GetModuleHandleW(NULL);
            HICON hBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
            HICON hSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
            if (hBig)   SendMessageW(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)hBig);
            if (hSmall) SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
        }

        ShowWindow(hList, SW_HIDE);

        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        RECT rc;
        GetWindowRect(hDlg, &rc);
        g_initW = rc.right - rc.left;
        g_initH = rc.bottom - rc.top;
/*底部状态栏高度修改在这里*/
        RECT rcDlu = { 0, 0, 0, 135 }; 
        MapDialogRect(hDlg, &rcDlu);
        g_expandedH = g_initH + rcDlu.bottom;

        int x, y;
        if (!LoadWindowPos(&x, &y)) {
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            x = (screenW - g_initW) / 2;
            y = (screenH - g_expandedH) / 2; 
        }
        
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        /* 新增：把窗口强行切成圆角，消除四个角的底色 */
        RECT rcWin;
        GetWindowRect(hDlg, &rcWin);
        HRGN hRgn = CreateRoundRectRgn(0, 0, rcWin.right - rcWin.left + 1, rcWin.bottom - rcWin.top + 1, 5, 5);
        SetWindowRgn(hDlg, hRgn, TRUE);
        
        g_listVisible = FALSE;
        g_selected = -1;
        g_matchCount = 0;

        /* 删除了 SetFocus 和 SetTimer，直接用最强力方式抢一次焦点 */
        ForceForeground(hDlg);

        /* --input= 预填，光标置开头。
         * 注意：必须放在 ForceForeground 之后，因为它内部会 EM_SETSEL -1,-1
         * 把光标移到末尾；也必须是 SetDlgItemTextW(hDlg, ...) 而不是 hEdit，
         * 第一个参数要传父窗口。 */
        if (g_prefill[0]) {
            SetDlgItemTextW(hDlg, ID_EDIT, g_prefill);
            SendMessageW(hEdit, EM_SETSEL, 0, 0);
        }

        return FALSE;
    }
/*顶部渐变 底部渐变 状态栏渐变 编辑框渐变*/
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hDlg, &rc);
        int midY = rc.top + (rc.bottom - rc.top) / 2;

        /* ===== 1. 对称渐变背景：顶灰 -> 中白 -> 底灰 ===== */
        TRIVERTEX vTop[2] = {
            { rc.left, rc.top, 0xbA00, 0xbA00, 0xbA00, 0x0000 },
            { rc.right, midY,  0xFF00, 0xFF00, 0xFF00, 0x0000 }
        };
        GRADIENT_RECT gTop = { 0, 1 };
        GradientFill(hdc, vTop, 2, &gTop, 1, GRADIENT_FILL_RECT_V);

        TRIVERTEX vBottom[2] = {
            { rc.left, midY,   0xFF00, 0xFF00, 0xFF00, 0x0000 },
            { rc.right, rc.bottom, 0xCA00, 0xCA00, 0xCA00, 0x0000 }
        };
        GRADIENT_RECT gBottom = { 0, 1 };
        GradientFill(hdc, vBottom, 2, &gBottom, 1, GRADIENT_FILL_RECT_V);

        /* ===== 2. 绘制输入框圆角底板 ===== */
        HBRUSH hWhite = (HBRUSH)GetStockObject(WHITE_BRUSH);
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200)); 
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hWhite);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

        RoundRect(hdc, g_rcEdit.left - 3, g_rcEdit.top - 2, 
                  g_rcEdit.right + 3, g_rcEdit.bottom + 3, 12, 12);

        SelectObject(hdc, hOldBrush);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);

        /* ===== 3. 绘制底部状态栏文字 ===== */
        if (g_listVisible) {
            HWND hList = GetDlgItem(hDlg, ID_LIST);
            RECT rcList;
            GetWindowRect(hList, &rcList);
            MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&rcList, 2);

            RECT rcStatus = rc;
            rcStatus.top = rcList.bottom; 

            SetBkMode(hdc, TRANSPARENT);
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontUI);
            SetTextColor(hdc, RGB(80, 80, 80)); 

            rcStatus.left += 10;
            rcStatus.right -= 10; 
            DrawTextW(hdc, g_statusText, -1, &rcStatus, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            SelectObject(hdc, hOldFont);
        }

        /* ===== 4. 绘制 1px 外边框 ===== */
        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(140, 140, 140));
        HPEN hOldBPen = (HPEN)SelectObject(hdc, hBorderPen);
        HBRUSH hOldBBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

        RoundRect(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, 5, 5);

        SelectObject(hdc, hOldBPen);
        SelectObject(hdc, hOldBBrush);
        DeleteObject(hBorderPen);

        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            if (!g_inModal) {
                PostMessageW(hDlg, WM_CLOSE, 0, 0);
            }
        }
        break;

    case WM_LBUTTONDOWN: {
        SendMessageW(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return TRUE;
    }

    case WM_EXITSIZEMOVE:
        SaveWindowPos(hDlg);
        return TRUE;

    case WM_QR_ACTIVATE:
        ForceForeground(hDlg);
        return TRUE;

    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lp;
        if (mis->CtlID == ID_LIST) {
            mis->itemHeight = 28; 
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlID != ID_LIST) break;
        if (dis->itemID == (UINT)-1) return TRUE;

        int idx = (int)SendMessageW(dis->hwndItem, LB_GETITEMDATA, dis->itemID, 0);
        if (idx < 0 || idx >= g_matchCount) return TRUE;

        HDC  hdc = dis->hDC;
        RECT rc  = dis->rcItem;
        BOOL sel = (dis->itemState & ODS_SELECTED) ? TRUE : FALSE;
        BOOL isFile = (g_matchCmds[idx][0] == 0);

        if (sel) {
            FillRect(hdc, &rc, CreateSolidBrush(RGB(14, 99, 156)));
        } else {
            FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        }

        HFONT hOld = (HFONT)SelectObject(hdc, g_hFontUI);
        SetBkMode(hdc, TRANSPARENT);

        SIZE sz = { 0, 0 };
        GetTextExtentPoint32W(hdc, g_matches[idx], (int)wcslen(g_matches[idx]), &sz);

        WCHAR num[8];
        wsprintfW(num, L"%d.", idx + 1);
        RECT rcNum = rc;
        rcNum.left += 8;
        
        SetTextColor(hdc, sel ? RGB(200, 200, 200) : RGB(160, 160, 160));
        DrawTextW(hdc, num, -1, &rcNum, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcKey = rc;
        rcKey.left += 30;
        if (isFile) {
            SetTextColor(hdc, sel ? RGB(255, 255, 255) : RGB(43, 87, 154));
            DrawTextW(hdc, g_matches[idx], -1, &rcKey,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            SetTextColor(hdc, sel ? RGB(255, 255, 255) : RGB(64, 64, 64));
            DrawTextW(hdc, g_matches[idx], -1, &rcKey,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT rcDesc = rc;
            rcDesc.left += 30 + sz.cx + 12;
            rcDesc.right -= 8;
            
            SetTextColor(hdc, sel ? RGB(224, 224, 224) : RGB(130, 130, 130));
            DrawTextW(hdc, g_matchDescs[idx], -1, &rcDesc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        SelectObject(hdc, hOld);
        return TRUE;
    }

    case WM_CTLCOLOREDIT: {
        if ((HWND)lp == GetDlgItem(hDlg, ID_EDIT)) {
            SetBkColor((HDC)wp, RGB(255, 255, 255));
            SetTextColor((HDC)wp, RGB(0, 0, 0)); 
            return (INT_PTR)GetStockObject(WHITE_BRUSH);
        }
        break;
    }

    case WM_COMMAND: {
        WORD id   = LOWORD(wp);
        WORD code = HIWORD(wp);

        if (id == ID_EDIT && code == EN_CHANGE) {
            g_selected = -1;
            SendMessageW(GetDlgItem(hDlg, ID_LIST),
                         LB_SETCURSEL, (WPARAM)-1, 0);
            UpdateSearch(hDlg);
            return TRUE;
        }

        if (id == ID_LIST && code == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(GetDlgItem(hDlg, ID_LIST),
                                        LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_matchCount)
                g_selected = sel;
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            UpdateStatusBar(hDlg);
            return TRUE;
        }

        if (id == ID_LIST && code == LBN_DBLCLK) {
            int sel = (int)SendMessageW(GetDlgItem(hDlg, ID_LIST),
                                        LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_matchCount) {
                g_selected = sel;
                PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
            }
            return TRUE;
        }

        if (id == IDOK) {
            ExecuteInput(hDlg, code == 1);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        if (id == ID_DATA) {
            HINSTANCE hi = ShellExecuteW(hDlg, L"open", g_dataFile,
                                         NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)hi <= 32) {
                WCHAR msg[800];
                wsprintfW(msg, L"无法打开数据文件：\n%s\n\n错误码：%d",
                          g_dataFile, (int)(INT_PTR)hi);
                g_inModal = TRUE;
                MessageBoxW(hDlg, msg, L"错误", MB_ICONERROR);
                g_inModal = FALSE;
            }
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }

        if (id == ID_HELP) {
            g_inModal = TRUE;
            MessageBoxW(hDlg, kHelpText, L"帮助", MB_ICONINFORMATION);
            g_inModal = FALSE;
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }

        if (id == ID_RESETPOS) {
            WCHAR iniPath[MAX_PATH];
            GetIniPath(iniPath, MAX_PATH);
            WritePrivateProfileStringW(L"Window", L"X", NULL, iniPath);
            WritePrivateProfileStringW(L"Window", L"Y", NULL, iniPath);
            
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            int x = (screenW - g_initW) / 2;
            int y = (screenH - g_expandedH) / 2;
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return TRUE;
        }

        if (id == ID_OPEN_FOLDER) {
            if (g_selected >= 0 && g_selected < g_matchCount &&
                g_matchCmds[g_selected][0] == 0) {
                /* 仅对 Everything 文件结果生效 */
                WCHAR arg[2048];
                wsprintfW(arg, L"/select,\"%s\"", g_matchFullPaths[g_selected]);
                ShellExecuteW(hDlg, L"open", L"explorer.exe",
                              arg, NULL, SW_SHOWNORMAL);
            }
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }
        return TRUE;
    }

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* =========================================================
 *  程序入口
 * ========================================================= */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev,
                   LPSTR lpCmdLine, int nShow)
{
    WCHAR path[MAX_PATH];
    WCHAR dataFile[MAX_PATH * 2];
    WCHAR *slash;
    DWORD  n;
    INT_PTR r;
    int    argc = 0;
    LPWSTR *argv;
    WCHAR  customData[MAX_PATH * 2] = { 0 };
    BOOL   hasCustomData = FALSE;
    BOOL   wantHelp = FALSE;
    int    i;

    (void)hPrev; (void)lpCmdLine; (void)nShow;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-h") == 0 ||
            _wcsicmp(argv[i], L"--help") == 0 ||
            _wcsicmp(argv[i], L"/?") == 0) {
            wantHelp = TRUE;
        } else if (_wcsicmp(argv[i], L"--no-fallback") == 0) {
            g_noFallback = TRUE;
        } else if (wcsncmp(argv[i], L"--data=", 7) == 0 ||
                   wcsncmp(argv[i], L"--date=", 7) == 0) {
            wcsncpy(customData, argv[i] + 7, MAX_PATH * 2 - 1);
            customData[MAX_PATH * 2 - 1] = 0;
            TrimW(customData);
            if (customData[0]) hasCustomData = TRUE;
        } else if (wcsncmp(argv[i], L"--input=", 8) == 0) {
            wcsncpy(g_prefill, argv[i] + 8, MAX_INPUT - 1);
            g_prefill[MAX_INPUT - 1] = 0;
        } else if (!g_input[0]) {
            wcsncpy(g_input, argv[i], MAX_INPUT - 1);
            g_input[MAX_INPUT - 1] = 0;
        } else if (g_argCount < MAX_ARGS) {
            wcsncpy(g_args[g_argCount], argv[i], MAX_INPUT - 1);
            g_args[g_argCount][MAX_INPUT - 1] = 0;
            g_argCount++;
        }
    }

    HANDLE hMutex = NULL;
    if (!g_input[0] && !wantHelp) {
        hMutex = CreateMutexW(NULL, FALSE, L"QuickRun_SingleInstance_Mutex");
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND hExisting = NULL;
            for (i = 0; i < 20 && !hExisting; i++) {
                hExisting = FindWindowW(NULL, L"QuickRun");
                if (!hExisting) Sleep(30);
            }
            if (hExisting) PostMessageW(hExisting, WM_QR_ACTIVATE, 0, 0);
            CloseHandle(hMutex);
            if (argv) LocalFree(argv);
            return 0;
        }
    }

    if (wantHelp) {
        MessageBoxW(NULL, kHelpText, L"帮助", MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        if (argv) LocalFree(argv);
        return 0;
    }

    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        if (hMutex) CloseHandle(hMutex);
        if (argv) LocalFree(argv);
        return 0;
    }
    slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    else       path[0] = 0;

    if (hasCustomData) {
        if (customData[0] == L'\\' ||
            (customData[0] && customData[1] == L':')) {
            wcsncpy(dataFile, customData, MAX_PATH * 2 - 1);
        } else {
            wcsncpy(dataFile, path, MAX_PATH * 2 - 1);
            wcsncat(dataFile, customData, MAX_PATH * 2 - 1 - wcslen(dataFile));
        }
    } else {
        wcsncpy(dataFile, path, MAX_PATH * 2 - 1);
        wcsncat(dataFile, L"QuickRun.txt", MAX_PATH * 2 - 1 - wcslen(dataFile));
    }
    dataFile[MAX_PATH * 2 - 1] = 0;

    if (GetFullPathNameW(dataFile, MAX_PATH * 2, g_dataFile, NULL) == 0) {
        wcsncpy(g_dataFile, dataFile, MAX_PATH * 2 - 1);
        g_dataFile[MAX_PATH * 2 - 1] = 0;
    }

    SetCurrentDirectoryW(path);

    EvLoad();

    if (!LoadDataFile(g_dataFile)) {
        if (!g_input[0]) {
            MessageBoxW(NULL, L"未找到或无法读取数据文件。", L"错误", MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            if (argv) LocalFree(argv);
            EvUnload();
            return 0;
        }
    }

    if (!g_input[0]) {
        g_executed = FALSE;
        BuildTemplate();
        r = DialogBoxIndirectParamW(hInst, (LPDLGTEMPLATE)g_tpl.b, NULL, DlgProc, 0);
        
        if (g_hFontUI) DeleteObject(g_hFontUI);
        if (g_hFontEdit) DeleteObject(g_hFontEdit);
        
        if (hMutex) CloseHandle(hMutex);
        if (argv) LocalFree(argv);
        EvUnload();
        (void)r;
        return 0;
    }

    {
        if (g_argCount > 0) {
            BOOL matched = FALSE;
            for (i = 0; i < g_entryCount; i++) {
                if (_wcsicmp(g_entries[i].key, g_input) == 0) {
                    RunCommandEx(g_entries[i].cmd, g_entries[i].desc, g_entries[i].useCmd);
                    matched = TRUE;
                    break;
                }
            }
            if (!matched && !g_noFallback) {
                WCHAR full[2048] = { 0 };
                WCHAR params[2048] = { 0 };
                size_t flen = 0, plen = 0;
                int k;

                AppendQuotedArg(full, 2048, &flen, g_input);
                for (k = 0; k < g_argCount; k++) {
                    if (flen + 1 < 2048) { full[flen++] = L' '; full[flen] = 0; }
                    AppendQuotedArg(full, 2048, &flen, g_args[k]);

                    if (plen + 1 < 2048) params[plen++] = L'"';
                    {
                        const WCHAR *s = g_args[k];
                        while (*s && plen + 2 < 2048) {
                            if (*s == L'"' && plen + 3 < 2048)
                                params[plen++] = L'\\';
                            params[plen++] = *s++;
                        }
                    }
                    if (plen + 2 < 2048) {
                        params[plen++] = L'"';
                        params[plen++] = L' ';
                    }
                }
                params[plen] = 0;

                HINSTANCE hi = ShellExecuteW(NULL, L"open", g_input,
                                             g_argCount ? params : NULL,
                                             NULL, SW_SHOWNORMAL);
                if ((INT_PTR)hi <= 32) {
                    WCHAR cmdLine[2048];
                    wsprintfW(cmdLine, L"cmd /c %s || pause", full);
                    STARTUPINFOW si = { sizeof(si) };
                    PROCESS_INFORMATION pi = { 0 };
                    if (CreateProcessW(NULL, cmdLine, NULL, NULL,
                                       FALSE, 0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                }
            }
        } else {
            BOOL matched = FALSE;
            for (i = 0; i < g_entryCount; i++) {
                if (_wcsicmp(g_entries[i].key, g_input) == 0) {
                    RunCommandEx(g_entries[i].cmd, g_entries[i].desc, g_entries[i].useCmd);
                    matched = TRUE;
                    break;
                }
            }
            if (!matched && !g_noFallback) {
                HINSTANCE hi = ShellExecuteW(NULL, L"open", g_input, NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)hi <= 32) {
                    WCHAR cmdLine[2048];
                    wsprintfW(cmdLine, L"cmd /c %s || pause", g_input);
                    STARTUPINFOW si = { sizeof(si) };
                    PROCESS_INFORMATION pi = { 0 };
                    if (CreateProcessW(NULL, cmdLine, NULL, NULL,
                                       FALSE, 0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hThread);
                        CloseHandle(pi.hProcess);
                    }
                }
            }
        }
    }

    if (hMutex) CloseHandle(hMutex);
    if (argv) LocalFree(argv);
    EvUnload();
    return 0;
}