/* LangFix - actions on selected text: keyboard-layout conversion and Claude writing tools
   (fix, rewrite, translate, the user's own request), plus the hint popup and update check.

   Flow: hotkey -> capture thread copies the selection with Ctrl+C (or the line before the
   caret when nothing is selected) -> the menu or the hotkey picks an instruction -> AI
   thread calls the Claude Messages API over WinHTTP -> paste thread pastes the result with
   Ctrl+V and puts the user's clipboard back. Only the selected text is sent, and only
   when the user presses an AI hotkey. The API key is stored encrypted with DPAPI. */
#include "app.h"
#include <winhttp.h>
#include <wincrypt.h>

#define API_VERSION  L"2026-10-21"
#define DEFAULT_BASE L"https://api.anthropic.com"
#define FALLBACK_BETA L"server-side-fallback-2026-07-01"

DWORD aiEnabled = 1, aiModel, aiMenuHk, aiFixHk;
static WCHAR aiBase[256];

static const struct { const char* id; const WCHAR* label; int fallbacks; } AI_MODELS[NMODELS] = {
  { "claude-opus-5-5",   L"Claude Opus 5.5 \x2014 best quality (recommended)", 1 },
  { "claude-sonnet-5-5", L"Claude Sonnet 5.5 \x2014 faster", 1 },
  { "claude-haiku-5-5",  L"Claude Haiku 5.5 \x2014 fastest and cheapest", 0 },
};

static const WCHAR* const INSTR[] = {
  0,
  L"Fix spelling, grammar, punctuation and words typed on the wrong keyboard layout. "
  L"Keep the language, meaning, tone and wording otherwise unchanged.",
  L"Rewrite it so it reads clearly and naturally. Keep the language, the meaning and roughly the length.",
  L"Make it shorter and more concise. Keep the language and the meaning.",
  L"Rewrite it in a polite, professional tone. Keep the language and the meaning.",
  L"Rewrite it in a warm, friendly, casual tone. Keep the language and the meaning.",
};

/* Editors that copy the whole line when nothing is selected. */
static const WCHAR* const LINE_COPIERS[] = {
  L"code.exe", L"code - insiders.exe", L"cursor.exe", L"windsurf.exe", L"devenv.exe", L"sublime_text.exe",
  L"idea64.exe", L"studio64.exe", L"pycharm64.exe", L"webstorm64.exe", L"rider64.exe", L"clion64.exe",
  L"goland64.exe", L"phpstorm64.exe", L"rubymine64.exe", L"datagrip64.exe", 0
};

const WCHAR* ai_model_label(int i) { return AI_MODELS[i].label; }

void ai_load_settings(void) {
  aiEnabled = reg_dword(L"AiEnabled", 1);
  aiModel = reg_dword(L"AiModel", 0);
  if (aiModel >= NMODELS) aiModel = 0;
  aiMenuHk = reg_dword(L"AiMenuHotkey", VK_SPACE | (HOTKEYF_CONTROL | HOTKEYF_ALT) << 8);
  aiFixHk = reg_dword(L"AiFixHotkey", VK_RETURN | (HOTKEYF_CONTROL | HOTKEYF_ALT) << 8);
  if (!reg_str(L"AiBaseUrl", aiBase, 256)) wcopy(aiBase, DEFAULT_BASE, 256);
}

/* ---------- API key (DPAPI, bound to this Windows account) ---------- */

int ai_key_get(WCHAR* out, int max) {
  BYTE buf[2048];
  DWORD sz = sizeof buf;
  DATA_BLOB in, plain;
  out[0] = 0;
  if (!RegGetValueW(HKEY_CURRENT_USER, REGKEY, L"AiKey", RRF_RT_REG_BINARY, 0, buf, &sz)) {
    in.cbData = sz;
    in.pbData = buf;
    if (CryptUnprotectData(&in, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
      wcopy(out, (const WCHAR*)plain.pbData, max);
      SecureZeroMemory(plain.pbData, plain.cbData);
      LocalFree(plain.pbData);
    }
  }
  if (!out[0] && GetEnvironmentVariableW(L"ANTHROPIC_API_KEY", out, (DWORD)max) >= (DWORD)max) out[0] = 0;
  return out[0] != 0;
}

void ai_key_set(const WCHAR* key) {
  DATA_BLOB in, out;
  if (!key[0]) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, REGKEY, L"AiKey");
    return;
  }
  in.cbData = (DWORD)((wlen(key) + 1) * sizeof(WCHAR));
  in.pbData = (BYTE*)key;
  if (CryptProtectData(&in, L"LangFix API key", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
    RegSetKeyValueW(HKEY_CURRENT_USER, REGKEY, L"AiKey", REG_BINARY, out.pbData, out.cbData);
    LocalFree(out.pbData);
  }
}

/* ---------- hint popup near the caret ---------- */

static HWND  hHint;
static WCHAR hintText[512];
static HFONT hintFont;
static UINT  hintDpi;

static LRESULT CALLBACK HintProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  PAINTSTRUCT ps;
  RECT rc;
  HDC dc;
  HBRUSH bg;
  if (m == WM_NCHITTEST) return HTTRANSPARENT;
  if (m != WM_PAINT) return DefWindowProcW(h, m, w, l);
  dc = BeginPaint(h, &ps);
  GetClientRect(h, &rc);
  bg = CreateSolidBrush(RGB(32, 33, 36));
  FillRect(dc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(242, 242, 242));
  SelectObject(dc, hintFont);
  DrawTextW(dc, hintText, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
  EndPaint(h, &ps);
  return 0;
}

/* Screen point of the text caret in the foreground window (the mouse if unknown). */
static void caret_point(POINT* pt, int* height) {
  GUITHREADINFO gi;
  HWND fg = GetForegroundWindow();
  gi.cbSize = sizeof gi;
  *height = 0;
  if (fg && GetGUIThreadInfo(GetWindowThreadProcessId(fg, 0), &gi) && gi.hwndCaret &&
      gi.rcCaret.bottom > gi.rcCaret.top) {
    pt->x = gi.rcCaret.left;
    pt->y = gi.rcCaret.top;
    ClientToScreen(gi.hwndCaret, pt);
    *height = gi.rcCaret.bottom - gi.rcCaret.top;
    return;
  }
  GetCursorPos(pt);
  *height = 16;
}

void hint_show(const WCHAR* text, DWORD ms) {
  WNDCLASSW wc;
  POINT pt;
  SIZE sz;
  MONITORINFO mi;
  HDC dc;
  HRGN rgn;
  UINT dpi;
  int ch, pad, w, h, x, y;
  if (!hHint) {
    zero(&wc, sizeof wc);
    wc.lpfnWndProc = HintProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"LangFixHint";
    RegisterClassW(&wc);
    hHint = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, 0, 0, hInst, 0);
    SetLayeredWindowAttributes(hHint, 0, 238, LWA_ALPHA);
  }
  wcopy(hintText, text, 512);
  caret_point(&pt, &ch);
  SetWindowPos(hHint, HWND_TOPMOST, pt.x, pt.y, 1, 1, SWP_NOACTIVATE);   /* to read that monitor's DPI */
  dpi = GetDpiForWindow(hHint);
  if (!dpi) dpi = 96;
  if (dpi != hintDpi || !hintFont) {
    if (hintFont) DeleteObject(hintFont);
    hintFont = CreateFontW(-MulDiv(10, (int)dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    hintDpi = dpi;
  }
  dc = GetDC(hHint);
  SelectObject(dc, hintFont);
  GetTextExtentPoint32W(dc, hintText, wlen(hintText), &sz);
  ReleaseDC(hHint, dc);
  pad = MulDiv(12, (int)dpi, 96);
  w = sz.cx + 2 * pad;
  h = sz.cy + pad;
  x = pt.x;
  y = pt.y + ch + MulDiv(6, (int)dpi, 96);
  mi.cbSize = sizeof mi;
  if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = pt.y - h - MulDiv(6, (int)dpi, 96);
  }
  rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, h / 2, h / 2);
  SetWindowRgn(hHint, rgn, FALSE);
  SetWindowPos(hHint, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(hHint, 0, TRUE);
  UpdateWindow(hHint);
  KillTimer(hWnd, TIMER_HINT);
  if (ms) SetTimer(hWnd, TIMER_HINT, ms, 0);
}

void hint_hide(void) {
  KillTimer(hWnd, TIMER_HINT);
  if (hHint) ShowWindow(hHint, SW_HIDE);
}

/* ---------- clipboard ---------- */

typedef struct { UINT fmt; HGLOBAL h; } CLIPITEM;
static CLIPITEM saved[32];
static int nsaved;

static int clip_open(void) {
  int i;
  for (i = 0; i < 25; i++) {
    if (OpenClipboard(hWnd)) return 1;
    Sleep(20);
  }
  return 0;
}

/* Keeps our temporary clipboard contents out of clipboard history (Win+V). */
static void clip_mark_private(void) {
  static UINT cf;
  HGLOBAL h;
  if (!cf) cf = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
  if (cf && (h = GlobalAlloc(GMEM_MOVEABLE, 4)) != 0 && !SetClipboardData(cf, h)) GlobalFree(h);
}

/* Copy every memory-based format of the open clipboard so it can be put back. */
static void clip_save(void) {
  UINT f = 0;
  HANDLE h;
  SIZE_T sz;
  void *s, *d;
  HGLOBAL copy;
  nsaved = 0;
  while ((f = EnumClipboardFormats(f)) != 0 && nsaved < 32) {
    if (f == CF_BITMAP || f == CF_METAFILEPICT || f == CF_ENHMETAFILE || f == CF_PALETTE || f == CF_OWNERDISPLAY ||
        f == CF_DSPBITMAP || f == CF_DSPMETAFILEPICT || f == CF_DSPENHMETAFILE || f == CF_DSPTEXT)
      continue;                        /* GDI handles; the bitmap comes back from CF_DIB */
    if (!(h = GetClipboardData(f)) || !(sz = GlobalSize(h)) || sz > 64 * 1024 * 1024) continue;
    if (!(s = GlobalLock(h))) continue;
    if ((copy = GlobalAlloc(GMEM_MOVEABLE, sz)) != 0 && (d = GlobalLock(copy)) != 0) {
      memcpy(d, s, sz);
      GlobalUnlock(copy);
      saved[nsaved].fmt = f;
      saved[nsaved].h = copy;
      nsaved++;
    }
    GlobalUnlock(h);
  }
}

static void clip_discard(void) {
  int i;
  for (i = 0; i < nsaved; i++) GlobalFree(saved[i].h);
  nsaved = 0;
}

static void clip_restore(void) {
  int i;
  if (!clip_open()) { clip_discard(); return; }
  EmptyClipboard();
  for (i = 0; i < nsaved; i++)
    if (!SetClipboardData(saved[i].fmt, saved[i].h)) GlobalFree(saved[i].h);
  nsaved = 0;
  CloseClipboard();
}

static int clip_set_text(const WCHAR* t) {
  HGLOBAL h;
  WCHAR* d;
  int n = wlen(t), ok = 0;
  if (!clip_open()) return 0;
  EmptyClipboard();
  if ((h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * sizeof(WCHAR))) != 0 && (d = (WCHAR*)GlobalLock(h)) != 0) {
    wcopy(d, t, n + 1);
    GlobalUnlock(h);
    ok = SetClipboardData(CF_UNICODETEXT, h) != 0;
    if (!ok) GlobalFree(h);
    clip_mark_private();
  }
  CloseClipboard();
  return ok;
}

static WCHAR* clip_get_text(void) {
  HANDLE h;
  const WCHAR* s;
  WCHAR* out = 0;
  int n;
  if (!clip_open()) return 0;
  if ((h = GetClipboardData(CF_UNICODETEXT)) != 0 && (s = (const WCHAR*)GlobalLock(h)) != 0) {
    n = wlen(s);
    if ((out = (WCHAR*)mem_alloc((SIZE_T)(n + 1) * sizeof(WCHAR))) != 0) wcopy(out, s, n + 1);
    GlobalUnlock(h);
  }
  CloseClipboard();
  return out;
}

static int clip_wait(DWORD seq0, int ms) {
  for (; ms > 0; ms -= 10) {
    if (GetClipboardSequenceNumber() != seq0) { Sleep(40); return 1; }
    Sleep(10);
  }
  return 0;
}

/* ---------- keys we send for copy / paste ---------- */

static int mods_down(void) {
  return (GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) |
          GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) < 0;
}

static void wait_mods_up(void) {
  int i;
  for (i = 0; i < 300 && mods_down(); i++) Sleep(10);
}

static void chord(WORD mod, WORD key) {
  INPUT in[4];
  int i;
  zero(in, sizeof in);
  for (i = 0; i < 4; i++) {
    in[i].type = INPUT_KEYBOARD;
    in[i].ki.dwExtraInfo = TAG_AI;
  }
  in[0].ki.wVk = mod;
  in[1].ki.wVk = key;
  in[2].ki.wVk = key;
  in[3].ki.wVk = mod;
  in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
  if (key == VK_HOME) { in[1].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; in[2].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY; }
  SendInput(4, in, sizeof(INPUT));
}

static void bring_to_front(HWND w) {
  HWND fg = GetForegroundWindow();
  DWORD me = GetCurrentThreadId(), ft = fg ? GetWindowThreadProcessId(fg, 0) : 0;
  if (!w || fg == w) return;
  if (ft && ft != me) AttachThreadInput(me, ft, TRUE);
  SetForegroundWindow(w);
  if (ft && ft != me) AttachThreadInput(me, ft, FALSE);
}

/* ---------- the job: one selected-text action at a time ---------- */

enum { ST_IDLE, ST_CAPTURE, ST_CHOOSE, ST_AI, ST_PASTE };

typedef struct {
  LONG   seq;
  int    ok, test;
  DWORD  ms;
  WCHAR* text;
  WCHAR* out;
  WCHAR  instr[700];
  WCHAR  err[200];
} AIREQ;

static struct {
  int    state, act, line;
  LONG   seq;
  HWND   target;                 /* the window with the selection */
  HWND   focus;                  /* the control inside it that had the keyboard focus */
  WCHAR* text;
  WCHAR* result;
} job;

/* Back to the window and the exact control the text came from (our menu took the focus). */
static void back_to_target(void) {
  DWORD me = GetCurrentThreadId(), tt = GetWindowThreadProcessId(job.target, 0);
  bring_to_front(job.target);
  if (!job.focus || !IsWindow(job.focus) || !tt) return;
  if (tt != me) AttachThreadInput(me, tt, TRUE);
  SetFocus(job.focus);
  if (tt != me) AttachThreadInput(me, tt, FALSE);
}

int ai_busy(void) { return job.state == ST_AI; }

static void job_end(void) {
  mem_free(job.text);
  mem_free(job.result);
  job.text = job.result = 0;
  job.state = ST_IDLE;
}

void ai_cancel(void) {
  if (job.state != ST_AI) return;
  job.seq++;                                   /* the reply, when it comes, is dropped */
  PostMessageW(hWnd, WM_JOB_END, (WPARAM)job.seq, 2);
}

static int one_line_with_break(const WCHAR* t) {
  int i, breaks = 0, n = wlen(t);
  for (i = 0; i < n; i++)
    if (t[i] == L'\n') breaks++;
  return breaks == 1 && n && t[n - 1] == L'\n';
}

static DWORD WINAPI capture_thread(void* p) {
  LONG seq = (LONG)(INT_PTR)p;
  WCHAR* t = 0;
  WCHAR exe[64];
  DWORD s0;
  int i, copier = 0;
  wait_mods_up();
  if (exe_of_window(job.target, exe, 64))
    for (i = 0; LINE_COPIERS[i]; i++)
      if (weq(exe, LINE_COPIERS[i])) copier = 1;
  if (clip_open()) {
    clip_save();
    CloseClipboard();
  }
  s0 = GetClipboardSequenceNumber();
  chord(VK_CONTROL, 'C');
  if (clip_wait(s0, 700)) t = clip_get_text();
  if (t && copier && one_line_with_break(t)) {   /* the editor copied the whole line: no selection */
    mem_free(t);
    t = 0;
  }
  if ((!t || !t[0]) && job.act != ACT_SELCONV) {  /* nothing selected: take the line up to the caret */
    mem_free(t);
    t = 0;
    s0 = GetClipboardSequenceNumber();
    chord(VK_SHIFT, VK_HOME);
    Sleep(60);
    chord(VK_CONTROL, 'C');
    if (clip_wait(s0, 700)) t = clip_get_text();
    job.line = 1;
  }
  PostMessageW(hWnd, WM_SEL_DONE, (WPARAM)seq, (LPARAM)t);
  return 0;
}

static DWORD WINAPI paste_thread(void* p) {
  LONG seq = (LONG)(INT_PTR)p;
  int pasted = 0;
  wait_mods_up();
  Sleep(30);
  if (GetForegroundWindow() == job.target && clip_set_text(job.result)) {
    chord(VK_CONTROL, 'V');
    Sleep(300);                                  /* let the app read it before we restore */
    clip_restore();
    pasted = 1;
  } else if (clip_set_text(job.result)) {
    clip_discard();                              /* the window is gone: leave the result to paste */
  }
  PostMessageW(hWnd, WM_JOB_END, (WPARAM)seq, pasted);
  return 0;
}

static void paste_start(void) {
  job.state = ST_PASTE;
  CloseHandle(CreateThread(0, 0, paste_thread, (void*)(INT_PTR)job.seq, 0, 0));
}

void sel_start(int act) {
  GUITHREADINFO gi;
  WCHAR key[256];
  if (job.state != ST_IDLE) {
    hint_show(L"LangFix is still working on the last request\x2026", 1500);
    return;
  }
  if ((act == ACT_AIFIX || act == ACT_AIMENU) && !ai_key_get(key, 256)) {
    hint_show(L"Add your Claude API key in LangFix \x2192 AI to use the AI tools", 3500);
    ui_show(PAGE_AI);
    return;
  }
  SecureZeroMemory(key, sizeof key);
  job.state = ST_CAPTURE;
  job.act = act;
  job.line = 0;
  job.seq++;
  job.target = GetForegroundWindow();
  gi.cbSize = sizeof gi;
  job.focus = GetGUIThreadInfo(GetWindowThreadProcessId(job.target, 0), &gi) ? gi.hwndFocus : 0;
  CloseHandle(CreateThread(0, 0, capture_thread, (void*)(INT_PTR)job.seq, 0, 0));
}

/* ---------- JSON (UTF-8) ---------- */

typedef struct { char* p; int n, cap; } SB;

static void sb_put(SB* b, const char* s, int n) {
  char* np;
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2 + 256;
    np = b->p ? (char*)HeapReAlloc(GetProcessHeap(), 0, b->p, (SIZE_T)b->cap) : (char*)mem_alloc((SIZE_T)b->cap);
    if (!np) return;
    b->p = np;
  }
  memcpy(b->p + b->n, s, (SIZE_T)n);
  b->n += n;
  b->p[b->n] = 0;
}

static void sb_str(SB* b, const char* s) { sb_put(b, s, lstrlenA(s)); }

/* A JSON string literal from UTF-16 text. */
static void sb_json(SB* b, const WCHAR* w) {
  char u[16];
  int i, j, n;
  sb_put(b, "\"", 1);
  for (i = 0; w[i]; ) {
    WCHAR c = w[i];
    if (c == L'"') { sb_put(b, "\\\"", 2); i++; }
    else if (c == L'\\') { sb_put(b, "\\\\", 2); i++; }
    else if (c == L'\n') { sb_put(b, "\\n", 2); i++; }
    else if (c == L'\r') { sb_put(b, "\\r", 2); i++; }
    else if (c == L'\t') { sb_put(b, "\\t", 2); i++; }
    else if (c < 0x20) { wsprintfA(u, "\\u%04x", c); sb_put(b, u, 6); i++; }
    else if (c < 0x80) { u[0] = (char)c; sb_put(b, u, 1); i++; }
    else {                                       /* a run of non-ASCII text (keeps surrogate pairs together) */
      for (j = i; w[j] >= 0x80; j++) {}
      n = WideCharToMultiByte(CP_UTF8, 0, w + i, j - i, 0, 0, 0, 0);
      if (n > 0) {
        char* tmp = (char*)mem_alloc((SIZE_T)n);
        if (tmp) {
          WideCharToMultiByte(CP_UTF8, 0, w + i, j - i, tmp, n, 0, 0);
          sb_put(b, tmp, n);
          mem_free(tmp);
        }
      }
      i = j;
    }
  }
  sb_put(b, "\"", 1);
}

static const char* js_ws(const char* p, const char* e) {
  while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
  return p;
}

static const char* js_skip_str(const char* p, const char* e) {
  for (p++; p < e && *p != '"'; p++)
    if (*p == '\\') p++;
  return p < e ? p + 1 : e;
}

/* Skips one JSON value. */
static const char* js_skip(const char* p, const char* e) {
  int depth = 0;
  p = js_ws(p, e);
  if (p >= e) return e;
  if (*p == '"') return js_skip_str(p, e);
  if (*p != '{' && *p != '[') {
    while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
  }
  while (p < e) {
    if (*p == '"') { p = js_skip_str(p, e); continue; }
    if (*p == '{' || *p == '[') depth++;
    else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
    p++;
  }
  return e;
}

/* The value of `key` in the object starting at obj, or 0. */
static const char* js_key(const char* obj, const char* e, const char* key) {
  const char* p = js_ws(obj, e);
  const char* k;
  int n = lstrlenA(key), match;
  if (p >= e || *p != '{') return 0;
  for (p++;;) {
    p = js_ws(p, e);
    if (p >= e || *p != '"') return 0;
    k = p + 1;
    p = js_skip_str(p, e);
    match = p - 1 - k == n && CompareStringA(LOCALE_INVARIANT, 0, k, n, key, n) == CSTR_EQUAL;
    p = js_ws(p, e);
    if (p >= e || *p != ':') return 0;
    p = js_ws(p + 1, e);
    if (match) return p;
    p = js_ws(js_skip(p, e), e);
    if (p >= e || *p != ',') return 0;
    p++;
  }
}

static int js_is(const char* p, const char* e, const char* lit) {
  int n = lstrlenA(lit);
  return p && e - p >= n && CompareStringA(LOCALE_INVARIANT, 0, p, n, lit, n) == CSTR_EQUAL;
}

static int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* Decodes the JSON string at p into newly allocated UTF-16 (caller frees). */
static WCHAR* js_wstr(const char* p, const char* e) {
  char* u;
  WCHAR* w;
  unsigned cp, lo;
  int n = 0, wn, k;
  const char* q;
  if (!p || p >= e || *p != '"') return 0;
  if (!(u = (char*)mem_alloc((SIZE_T)(e - p) + 8))) return 0;
  for (q = p + 1; q < e && *q != '"'; q++) {
    if (*q != '\\') { u[n++] = *q; continue; }
    if (++q >= e) break;
    switch (*q) {
    case 'n': u[n++] = '\n'; break;
    case 'r': u[n++] = '\r'; break;
    case 't': u[n++] = '\t'; break;
    case 'b': u[n++] = '\b'; break;
    case 'f': u[n++] = '\f'; break;
    case 'u':
      for (cp = 0, k = 1; k <= 4 && q + k < e && hexv(q[k]) >= 0; k++) cp = cp * 16 + (unsigned)hexv(q[k]);
      q += 4;
      if (cp >= 0xD800 && cp < 0xDC00 && q + 6 < e && q[1] == '\\' && q[2] == 'u') {
        for (lo = 0, k = 3; k <= 6 && hexv(q[k]) >= 0; k++) lo = lo * 16 + (unsigned)hexv(q[k]);
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        q += 6;
      }
      if (cp < 0x80) u[n++] = (char)cp;
      else if (cp < 0x800) { u[n++] = (char)(0xC0 | cp >> 6); u[n++] = (char)(0x80 | (cp & 63)); }
      else if (cp < 0x10000) { u[n++] = (char)(0xE0 | cp >> 12); u[n++] = (char)(0x80 | (cp >> 6 & 63)); u[n++] = (char)(0x80 | (cp & 63)); }
      else { u[n++] = (char)(0xF0 | cp >> 18); u[n++] = (char)(0x80 | (cp >> 12 & 63)); u[n++] = (char)(0x80 | (cp >> 6 & 63)); u[n++] = (char)(0x80 | (cp & 63)); }
      break;
    default: u[n++] = *q;                        /* \" \\ \/ */
    }
  }
  wn = MultiByteToWideChar(CP_UTF8, 0, u, n, 0, 0);
  if ((w = (WCHAR*)mem_alloc((SIZE_T)(wn + 1) * sizeof(WCHAR))) != 0) {
    MultiByteToWideChar(CP_UTF8, 0, u, n, w, wn);
    w[wn] = 0;
  }
  mem_free(u);
  return w;
}

/* ---------- HTTP (WinHTTP) ---------- */

static int http_request(const WCHAR* method, const WCHAR* url, const WCHAR* headers, const char* body, DWORD bodyLen,
                        char** resp, DWORD* respLen, DWORD* status, int timeoutMs) {
  URL_COMPONENTS uc;
  WCHAR host[256], path[1024];
  HINTERNET hs = 0, hc = 0, hr = 0;
  DWORD sz, avail, got, cap = 0;
  char* buf = 0;
  int ok = 0;
  *resp = 0;
  *respLen = *status = 0;
  zero(&uc, sizeof uc);
  uc.dwStructSize = sizeof uc;
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 1024;
  if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
  if (!(hs = WinHttpOpen(L"LangFix/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0)))
    return 0;
  WinHttpSetTimeouts(hs, 10000, 10000, 30000, timeoutMs);
  if ((hc = WinHttpConnect(hs, host, uc.nPort, 0)) != 0 &&
      (hr = WinHttpOpenRequest(hc, method, path, 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)) != 0 &&
      WinHttpSendRequest(hr, headers, (DWORD)-1L, (void*)body, bodyLen, bodyLen, 0) &&
      WinHttpReceiveResponse(hr, 0)) {
    sz = sizeof *status;
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        status, &sz, WINHTTP_NO_HEADER_INDEX);
    ok = 1;
    while (WinHttpQueryDataAvailable(hr, &avail) && avail) {
      if (*respLen + avail + 1 > cap) {
        char* nb;
        cap = (*respLen + avail + 1) * 2;
        if (cap > 8 * 1024 * 1024) { ok = 0; break; }
        nb = buf ? (char*)HeapReAlloc(GetProcessHeap(), 0, buf, cap) : (char*)mem_alloc(cap);
        if (!nb) { ok = 0; break; }
        buf = nb;
      }
      if (!WinHttpReadData(hr, buf + *respLen, avail, &got) || !got) break;
      *respLen += got;
    }
    if (buf) buf[*respLen] = 0;
  }
  if (hr) WinHttpCloseHandle(hr);
  if (hc) WinHttpCloseHandle(hc);
  WinHttpCloseHandle(hs);
  if (!ok) { mem_free(buf); buf = 0; *respLen = 0; }
  *resp = buf;
  return ok;
}

/* ---------- Claude ---------- */

/* English names of the languages LangFix switches between ("English", "Russian", ...). */
static int target_languages(WCHAR (*names)[64]) {
  WCHAR nm[64];
  int g, j, n = 0;
  for (g = 0; g < g_nlay && n < MAXL; g++) {
    if (!g_lay[g].enabled || !g_lay[g].nloc) continue;
    if (!GetLocaleInfoEx(g_lay[g].loc[0], LOCALE_SENGLISHLANGUAGENAME, nm, 64)) continue;
    for (j = 0; j < n && !weq(names[j], nm); j++) {}
    if (j == n) wcopy(names[n++], nm, 64);
  }
  return n;
}

static void build_body(SB* b, const WCHAR* instr, const WCHAR* text) {
  static WCHAR sys[1200], user[MAXSEL + 900];
  WCHAR langs[MAXL][64];
  int i, n = target_languages(langs);
  wcopy(sys,
        L"You edit text for someone typing in a Windows app. The user message holds an instruction and the "
        L"text between <text> and </text>. Reply with only the resulting text: no quotes, labels, explanations "
        L"or markdown that the original doesn't use. Keep line breaks, lists and emojis unless the instruction "
        L"asks otherwise. Some words may have been typed on the wrong keyboard layout (for example \"ghbdtn\" "
        L"meaning \"\x043F\x0440\x0438\x0432\x0435\x0442\", or \"\x05D9\x05E7\x05DA\x05DA\x05DD\" meaning "
        L"\"hello\"); read them as the words the person meant.", 1200);
  if (n) {
    wcat(sys, L" They write in ", 1200);
    for (i = 0; i < n; i++) {
      if (i) wcat(sys, i == n - 1 ? L" and " : L", ", 1200);
      wcat(sys, langs[i], 1200);
    }
    wcat(sys, L".", 1200);
  }
  wcopy(user, L"Instruction: ", MAXSEL + 900);
  wcat(user, instr, MAXSEL + 900);
  wcat(user, L"\n\n<text>\n", MAXSEL + 900);
  wcat(user, text, MAXSEL + 900);
  wcat(user, L"\n</text>", MAXSEL + 900);

  sb_str(b, "{\"model\":\"");
  sb_str(b, AI_MODELS[aiModel].id);
  sb_str(b, "\",\"max_tokens\":16000,\"output_config\":{\"effort\":\"low\"},");
  if (AI_MODELS[aiModel].fallbacks) sb_str(b, "\"fallbacks\":\"default\",");
  sb_str(b, "\"system\":");
  sb_json(b, sys);
  sb_str(b, ",\"messages\":[{\"role\":\"user\",\"content\":");
  sb_json(b, user);
  sb_str(b, "}]}");
}

static void friendly_error(DWORD status, const char* resp, DWORD n, WCHAR* err, int max) {
  const char *e = resp + n, *eo, *msg;
  WCHAR* w = 0;
  switch (status) {
  case 401: wcopy(err, L"the API key was rejected \x2014 check it in LangFix \x2192 AI", max); return;
  case 402: wcopy(err, L"there is a billing problem on your Anthropic account", max); return;
  case 429: wcopy(err, L"rate limit reached \x2014 try again in a moment", max); return;
  case 529: case 503: wcopy(err, L"Claude is overloaded right now \x2014 try again shortly", max); return;
  }
  if (resp && (eo = js_key(resp, e, "error")) != 0 && (msg = js_key(eo, e, "message")) != 0) w = js_wstr(msg, e);
  if (w) { wcopy(err, w, max); mem_free(w); }
  else wsprintfW(err, L"the API answered with HTTP %lu", status);
}

/* Runs on a worker thread. Returns 1 with *out (caller frees) or 0 with err. */
static int claude_call(const WCHAR* instr, const WCHAR* text, WCHAR** out, WCHAR* err, int errMax) {
  static const WCHAR fmt[] = L"content-type: application/json\r\nx-api-key: %s\r\nanthropic-version: " API_VERSION L"\r\n%s";
  WCHAR key[256], url[320], headers[600];
  SB body = { 0, 0, 0 };
  char* resp = 0;
  const char *e, *root, *p, *el, *ty;
  DWORD n = 0, status = 0;
  int tries, ok = 0, len;
  WCHAR *piece, *acc = 0;
  *out = 0;
  err[0] = 0;
  if (!ai_key_get(key, 256)) { wcopy(err, L"no API key", errMax); return 0; }
  wcopy(url, aiBase, 300);
  len = wlen(url);
  while (len && url[len - 1] == L'/') url[--len] = 0;
  wcat(url, L"/v1/messages", 320);
  wsprintfW(headers, fmt, key, AI_MODELS[aiModel].fallbacks ? L"anthropic-beta: " FALLBACK_BETA L"\r\n" : L"");
  SecureZeroMemory(key, sizeof key);
  build_body(&body, instr, text);

  for (tries = 0; tries < 2; tries++) {      /* one retry for overload / rate limits / network */
    mem_free(resp);
    resp = 0;
    if (!http_request(L"POST", url, headers, body.p, (DWORD)body.n, &resp, &n, &status, 90000)) {
      wcopy(err, L"can't reach the Claude API \x2014 check your internet connection", errMax);
      if (!tries) { Sleep(1000); continue; }
      break;
    }
    if ((status == 429 || status == 500 || status == 503 || status == 529) && !tries) { Sleep(1500); continue; }
    break;
  }
  SecureZeroMemory(headers, sizeof headers);
  mem_free(body.p);
  if (!resp) return 0;
  e = resp + n;
  root = js_ws(resp, e);
  if (status != 200) {
    friendly_error(status, root, (DWORD)(e - root), err, errMax);
  } else if (js_is(js_key(root, e, "stop_reason"), e, "\"refusal\"")) {
    wcopy(err, L"Claude declined to change this text", errMax);
  } else if ((p = js_key(root, e, "content")) != 0 && *p == '[') {
    /* concatenate the text blocks; thinking (and fallback) blocks are skipped */
    for (p = js_ws(p + 1, e); p < e && *p == '{'; ) {
      el = p;
      ty = js_key(el, e, "type");
      if (js_is(ty, e, "\"text\"") && (piece = js_wstr(js_key(el, e, "text"), e)) != 0) {
        int a = acc ? wlen(acc) : 0, b = wlen(piece);
        WCHAR* nb = (WCHAR*)mem_alloc((SIZE_T)(a + b + 1) * sizeof(WCHAR));
        if (nb) {
          if (acc) wcopy(nb, acc, a + 1);
          wcopy(nb + a, piece, b + 1);
        }
        mem_free(acc);
        mem_free(piece);
        acc = nb;
      }
      p = js_ws(js_skip(el, e), e);
      if (p < e && *p == ',') p = js_ws(p + 1, e);
    }
    if (acc && acc[0]) { *out = acc; ok = 1; }
    else { mem_free(acc); wcopy(err, L"Claude returned no text", errMax); }
  } else {
    wcopy(err, L"unexpected answer from the API", errMax);
  }
  mem_free(resp);
  return ok;
}

static DWORD WINAPI ai_thread(void* p) {
  AIREQ* r = (AIREQ*)p;
  DWORD t0 = GetTickCount();
  r->ok = claude_call(r->instr, r->text, &r->out, r->err, 200);
  r->ms = GetTickCount() - t0;
  PostMessageW(hWnd, WM_AI_DONE, 0, (LPARAM)r);
  return 0;
}

static void ai_start(const WCHAR* instr) {
  AIREQ* r = (AIREQ*)mem_alloc(sizeof *r);
  int n = wlen(job.text);
  if (!r || !(r->text = (WCHAR*)mem_alloc((SIZE_T)(n + 1) * sizeof(WCHAR)))) {
    mem_free(r);
    clip_restore();
    job_end();
    return;
  }
  wcopy(r->text, job.text, n + 1);
  wcopy(r->instr, instr, 700);
  r->seq = job.seq;
  job.state = ST_AI;
  hint_show(L"\x2728  Claude is working\x2026   Esc cancels", 0);
  CloseHandle(CreateThread(0, 0, ai_thread, r, 0, 0));
}

void ai_test(void) {
  AIREQ* r = (AIREQ*)mem_alloc(sizeof *r);
  if (!r || !(r->text = (WCHAR*)mem_alloc(8 * sizeof(WCHAR)))) { mem_free(r); return; }
  wcopy(r->text, L"OK", 8);
  wcopy(r->instr, L"Reply with exactly the text as it is.", 700);
  r->test = 1;
  CloseHandle(CreateThread(0, 0, ai_thread, r, 0, 0));
}

static int is_ws(WCHAR c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

/* The reply trimmed, with the selection's own leading/trailing whitespace and CRLF line ends. */
static WCHAR* fit_reply(const WCHAR* orig, const WCHAR* res) {
  int n = wlen(orig), a = 0, b = n, ra = 0, rb = wlen(res), i, o = 0;
  WCHAR* out;
  while (a < n && is_ws(orig[a])) a++;
  while (b > a && is_ws(orig[b - 1])) b--;
  while (ra < rb && is_ws(res[ra])) ra++;
  while (rb > ra && is_ws(res[rb - 1])) rb--;
  if (rb - ra >= 13 && CompareStringOrdinal(res + ra, 6, L"<text>", 6, TRUE) == CSTR_EQUAL &&
      CompareStringOrdinal(res + rb - 7, 7, L"</text>", 7, TRUE) == CSTR_EQUAL) {
    ra += 6;
    rb -= 7;
    while (ra < rb && is_ws(res[ra])) ra++;
    while (rb > ra && is_ws(res[rb - 1])) rb--;
  }
  if (!(out = (WCHAR*)mem_alloc((SIZE_T)(a + 2 * (rb - ra) + (n - b) + 1) * sizeof(WCHAR)))) return 0;
  for (i = 0; i < a; i++) out[o++] = orig[i];
  for (i = ra; i < rb; i++) {
    if (res[i] == L'\n' && (i == ra || res[i - 1] != L'\r')) out[o++] = L'\r';
    out[o++] = res[i];
  }
  for (i = b; i < n; i++) out[o++] = orig[i];
  out[o] = 0;
  return out;
}

static void do_convert(void) {
  int n = wlen(job.text), t;
  WCHAR* out = (WCHAR*)mem_alloc((SIZE_T)(n + 1) * sizeof(WCHAR));
  t = out ? eng_convert_text(job.text, n, out) : -1;
  if (t < 0) {
    mem_free(out);
    clip_restore();
    job_end();
    hint_show(L"Nothing to convert in this text", 2000);
    return;
  }
  job.result = out;
  convCount++;
  PostMessageW(hWnd, WM_STATS, 0, 0);
  switch_layout_in(job.target, t);
  paste_start();
}

static void ai_menu(void) {
  WCHAR langs[MAXL][64], label[96], instr[700];
  HMENU m = CreatePopupMenu();
  POINT pt;
  int i, nl, cmd, ch;
  AppendMenuW(m, MF_STRING, 1, L"&Fix spelling and grammar");
  AppendMenuW(m, MF_STRING, 2, L"&Improve writing");
  AppendMenuW(m, MF_STRING, 3, L"Make &shorter");
  AppendMenuW(m, MF_STRING, 4, L"Make more f&ormal");
  AppendMenuW(m, MF_STRING, 5, L"Make more friend&ly");
  AppendMenuW(m, MF_SEPARATOR, 0, 0);
  nl = target_languages(langs);
  for (i = 0; i < nl; i++) {
    wsprintfW(label, L"&Translate to %s", langs[i]);
    AppendMenuW(m, MF_STRING, 10 + i, label);
  }
  AppendMenuW(m, MF_SEPARATOR, 0, 0);
  AppendMenuW(m, MF_STRING, 30, L"Convert keyboard la&yout (no AI)");
  AppendMenuW(m, MF_STRING, 31, L"&Ask Claude\x2026");
  SetMenuDefaultItem(m, 1, FALSE);
  caret_point(&pt, &ch);
  job.state = ST_CHOOSE;
  bring_to_front(hWnd);
  PostMessageW(hWnd, WM_KEYDOWN, VK_DOWN, 0);     /* highlight the first item: Enter picks "Fix" */
  cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y + ch, 0, hWnd, 0);
  PostMessageW(hWnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  if (cmd == 31) {
    if (!ui_ask(0, instr, 700)) cmd = 0;
  } else if (cmd >= 10 && cmd < 10 + nl) {
    wsprintfW(instr, L"Translate it into %s. Keep the tone, names and formatting.", langs[cmd - 10]);
  } else if (cmd >= 1 && cmd <= 5) {
    wcopy(instr, INSTR[cmd], 700);
  }
  back_to_target();
  if (!cmd) {
    clip_restore();
    job_end();
  } else if (cmd == 30) {
    do_convert();
  } else {
    ai_start(instr);
  }
}

LRESULT ai_message(UINT m, WPARAM w, LPARAM l) {
  WCHAR msg[260];
  AIREQ* r;
  WCHAR* t;
  switch (m) {
  case WM_SEL:
    sel_start((int)w);
    break;
  case WM_SEL_DONE:
    t = (WCHAR*)l;
    if ((LONG)w != job.seq || job.state != ST_CAPTURE) { mem_free(t); break; }
    if (!t || !t[0] || wlen(t) > MAXSEL) {
      clip_restore();
      job_end();
      hint_show(t && t[0] ? L"That selection is too long" : L"Select some text first", 2500);
      mem_free(t);
      break;
    }
    job.text = t;
    if (job.act == ACT_SELCONV) do_convert();
    else if (job.act == ACT_AIFIX) ai_start(INSTR[1]);
    else ai_menu();
    break;
  case WM_AI_DONE:
    r = (AIREQ*)l;
    if (r->test) {
      if (r->ok) wsprintfW(msg, L"Connected \x2014 Claude answered in %lu.%lu s.", r->ms / 1000, r->ms % 1000 / 100);
      else wsprintfW(msg, L"Not working: %s.", r->err);
      ui_ai_result(msg);
    } else if (r->seq == job.seq && job.state == ST_AI) {
      hint_hide();
      if (!r->ok) {
        clip_restore();
        job_end();
        wsprintfW(msg, L"Claude: %s", r->err);
        hint_show(msg, 5000);
      } else if ((job.result = fit_reply(job.text, r->out)) != 0) {
        aiCount++;
        PostMessageW(hWnd, WM_STATS, 0, 0);
        back_to_target();
        paste_start();
      } else {
        clip_restore();
        job_end();
      }
    }
    mem_free(r->text);
    mem_free(r->out);
    mem_free(r);
    break;
  case WM_JOB_END:
    if ((LONG)w != job.seq) break;
    if (l == 2) {                                  /* cancelled with Esc */
      clip_restore();
      hint_show(L"Cancelled", 1000);
    } else if (!l) {
      hint_show(L"The window changed \x2014 the result is on the clipboard", 3500);
    }
    job_end();
    break;
  case WM_UPDATE:
    t = (WCHAR*)l;
    wsprintfW(msg, L"LangFix %s is available. Click here to open the download page.", t);
    balloon(L"LangFix update", msg);
    mem_free(t);
    break;
  }
  return 0;
}

/* ---------- update check (GitHub releases, at most once a day) ---------- */

static int version_num(const WCHAR* s) {
  int part[3] = { 0, 0, 0 }, i = 0;
  if (*s == L'v' || *s == L'V') s++;
  for (; *s && i < 3; s++) {
    if (*s >= L'0' && *s <= L'9') part[i] = part[i] * 10 + (*s - L'0');
    else if (*s == L'.') i++;
    else break;
  }
  return part[0] * 10000 + part[1] * 100 + part[2];
}

static DWORD WINAPI update_thread(void* p) {
  char* resp = 0;
  DWORD n = 0, status = 0;
  WCHAR* tag = 0;
  (void)p;
  if (http_request(L"GET", L"https://api.github.com/repos/alexeybe1kin/LangFix/releases/latest",
                   L"Accept: application/vnd.github+json\r\n", 0, 0, &resp, &n, &status, 15000) &&
      status == 200 && resp)
    tag = js_wstr(js_key(js_ws(resp, resp + n), resp + n, "tag_name"), resp + n);
  if (tag && version_num(tag) > version_num(APP_VERSION)) PostMessageW(hWnd, WM_UPDATE, 0, (LPARAM)tag);
  else mem_free(tag);
  mem_free(resp);
  return 0;
}

void update_check(BOOL force) {
  static DWORD checkedDay;
  FILETIME ft;
  ULARGE_INTEGER u;
  DWORD day;
  GetSystemTimeAsFileTime(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  day = (DWORD)(u.QuadPart / 864000000000ULL);
  if (!checkedDay) checkedDay = reg_dword(L"LastUpdateCheck", 0);
  if (!force && day == checkedDay) return;
  checkedDay = day;
  reg_set_dword(L"LastUpdateCheck", day);
  CloseHandle(CreateThread(0, 0, update_thread, 0, 0, 0));
}

void update_open(void) {
  ShellExecuteW(0, L"open", REPO_URL L"/releases/latest", 0, 0, SW_SHOWNORMAL);
}
