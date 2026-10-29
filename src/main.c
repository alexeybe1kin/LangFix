/* LangFix - fixes words typed in the wrong keyboard layout.

   A low-level keyboard hook records the keys of the word being typed. On Space the
   engine reads those keys through every installed layout; when another layout clearly
   fits, the word is erased, retyped as Unicode and the window's layout is switched.
   Keys the user presses while our replacement is in flight are held back and replayed
   afterwards, so fast typing never interleaves with the correction.

   Pause, both Shift keys or a custom hotkey convert the current/last word by hand, or
   undo an automatic fix and remember the word so it is left alone next time. Selected
   text can be converted or handed to Claude (ai.c); the settings window is in ui.c.

   "--tray" (used for autostart) starts without opening the window. No CRT. */
#include "app.h"
#include <shlobj.h>
#include <spellcheck.h>
#include <intrin.h>

/* ---------- no-CRT support ---------- */
#pragma function(memset, memcpy)
void* __cdecl memset(void* d, int c, size_t n) { __stosb((unsigned char*)d, (unsigned char)c, n); return d; }
void* __cdecl memcpy(void* d, const void* s, size_t n) { __movsb((unsigned char*)d, (const unsigned char*)s, n); return d; }

#define RUNKEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define TAG        0x4C460000u          /* dwExtraInfo of input we inject ('LF') */
#define TAG_MASK   0xFFFF0000u
#define TAG_REPLAY 0x8000u              /* replayed user key (vs. our own correction) */
#define VK_MASK    0xE8                 /* unassigned key: keeps a lone Alt from opening menus */
#define MAXQ       16
#define MAXHELD    128
#define CTX_MS     60000                /* the previous word counts as context this long */

enum { IDM_OPEN = 100, IDM_ENABLE, IDM_AUTOSTART, IDM_EXIT };

static const CLSID CLSID_SCF_       = {0x7AB36653,0x1796,0x484B,{0xBD,0xFA,0xE7,0x4F,0x1D,0xB7,0xC1,0xDC}};
static const IID   IID_SCF_         = {0x8E018A9D,0x2415,0x4677,{0xBF,0x08,0x79,0x4E,0xA6,0x1F,0x94,0xBB}};
static const CLSID CLSID_ShellLink_ = {0x00021401,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const IID   IID_IShellLinkW_ = {0x000214F9,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const IID   IID_IPersistFile_= {0x0000010B,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const GUID  FOLDERID_Programs_ = {0xA77F5D77,0x2E2B,0x44C3,{0xA6,0xA2,0xAB,0xA6,0x01,0x05,0x4A,0x51}};

HINSTANCE hInst;
HWND  hWnd;
HICON icoOn, icoOff, icoBig;
static HHOOK hkKb, hkMs;
static UINT  wmTaskbarCreated;
static NOTIFYICONDATAW nid;
static DWORD lastHookTime, hookInstallTick;
int   fixCount, undoCount, convCount, aiCount;
WCHAR g_hint[256];

/* settings (HKCU\Software\LangFix) */
BOOL  enabled = TRUE;
DWORD setPause = 1, setShifts = 1, setConvertHk, setToggleHk, setSens = 1, setHints = 1, setEnter,
      setUpdates = 1, setSelPause = 1, setSelHk;
static const int sensGain[3] = { 80, 64, 48 };   /* careful, balanced, eager */

/* ---------- small helpers ---------- */

int wlen(const WCHAR* s) { int n = 0; while (s[n]) n++; return n; }
void wcopy(WCHAR* d, const WCHAR* s, int max) {
  int i;
  for (i = 0; i < max - 1 && s[i]; i++) d[i] = s[i];
  d[i] = 0;
}
void wcat(WCHAR* d, const WCHAR* s, int max) { int n = wlen(d); wcopy(d + n, s, max - n); }
int weq(const WCHAR* a, const WCHAR* b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
void zero(void* p, int n) { BYTE* b = (BYTE*)p; while (n--) *b++ = 0; }
void* mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
void mem_free(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ---------- spellchecker worker thread ----------
   The Windows spellchecker runs on its own MTA thread; the hook thread hands it a
   batch and waits at most a few dozen ms, so a slow dictionary can never stall typing. */

static CRITICAL_SECTION cs;
static HANDLE evReq, evResp;
static LONG   reqSeq, doneSeq;
static int    qn, qmode[MAXQ], qres[MAXQ];
static WCHAR  qloc[MAXQ][LOCALE_NAME_MAX_LENGTH], qw[MAXQ][MAXW + 1];

static ISpellCheckerFactory* fac;
static struct { WCHAR loc[LOCALE_NAME_MAX_LENGTH]; ISpellChecker* sc; } chk[16];
static int nchk;

static ISpellChecker* checker(const WCHAR* loc) {
  int i;
  BOOL sup = FALSE;
  ISpellChecker* sc = 0;
  for (i = 0; i < nchk; i++)
    if (CompareStringOrdinal(chk[i].loc, -1, loc, -1, TRUE) == CSTR_EQUAL) return chk[i].sc;
  if (!fac || nchk == 16) return 0;
  if (SUCCEEDED(ISpellCheckerFactory_IsSupported(fac, loc, &sup)) && sup)
    ISpellCheckerFactory_CreateSpellChecker(fac, loc, &sc);
  wcopy(chk[nchk].loc, loc, LOCALE_NAME_MAX_LENGTH);
  chk[nchk].sc = sc;
  return chk[nchk++].sc;
}

static int spell_check(ISpellChecker* sc, const WCHAR* w) {
  IEnumSpellingError* e = 0;
  ISpellingError* err = 0;
  int r;
  if (!sc || FAILED(ISpellChecker_Check(sc, w, &e)) || !e) return -1;
  r = IEnumSpellingError_Next(e, &err) == S_OK ? 0 : 1;
  if (err) ISpellingError_Release(err);
  IEnumSpellingError_Release(e);
  return r;
}

/* 1 if the dictionary suggests a word one edit away from w (w is probably a typo). */
static int spell_typo(ISpellChecker* sc, const WCHAR* w) {
  IEnumString* es = 0;
  LPOLESTR s;
  ULONG got;
  int found = 0, n = wlen(w);
  if (!sc || FAILED(ISpellChecker_Suggest(sc, w, &es)) || !es) return -1;
  while (!found && IEnumString_Next(es, 1, &s, &got) == S_OK) {
    found = eng_one_edit(s, wlen(s), w, n);
    CoTaskMemFree(s);
  }
  IEnumString_Release(es);
  return found;
}

static DWORD WINAPI spell_thread(void* unused) {
  static WCHAR loc[MAXQ][LOCALE_NAME_MAX_LENGTH], w[MAXQ][MAXW + 1];
  int res[MAXQ], mode[MAXQ], n, i;
  LONG seq;
  (void)unused;
  CoInitializeEx(0, COINIT_MULTITHREADED);
  CoCreateInstance(&CLSID_SCF_, 0, CLSCTX_INPROC_SERVER, &IID_SCF_, (void**)&fac);
  for (;;) {
    WaitForSingleObject(evReq, INFINITE);
    EnterCriticalSection(&cs);
    seq = reqSeq;
    n = qn;
    for (i = 0; i < n; i++) {
      wcopy(loc[i], qloc[i], LOCALE_NAME_MAX_LENGTH);
      wcopy(w[i], qw[i], MAXW + 1);
      mode[i] = qmode[i];
    }
    LeaveCriticalSection(&cs);
    for (i = 0; i < n; i++)
      res[i] = mode[i] ? spell_typo(checker(loc[i]), w[i]) : spell_check(checker(loc[i]), w[i]);
    EnterCriticalSection(&cs);
    if (seq == reqSeq) {
      for (i = 0; i < n; i++) qres[i] = res[i];
      doneSeq = seq;
    }
    LeaveCriticalSection(&cs);
    SetEvent(evResp);
  }
}

int spell_ask(DICTQ* q, int nq, DWORD timeout) {
  int map[MAXQ], i, j, k, n = 0, r, *d;
  LONG seq;
  DWORD t0 = GetTickCount(), el;
  EnterCriticalSection(&cs);
  for (i = 0; i < nq; i++) {
    const LAYOUT* L = &g_lay[q[i].g];
    q[i].res = -1;
    for (j = 0; j < L->nloc && n < MAXQ; j++, n++) {
      wcopy(qloc[n], L->loc[j], LOCALE_NAME_MAX_LENGTH);
      for (k = 0; k < q[i].n && k < MAXW; k++) qw[n][k] = q[i].w[k];
      qw[n][k] = 0;
      qmode[n] = q[i].typo;
      map[n] = i;
    }
  }
  qn = n;
  seq = ++reqSeq;
  LeaveCriticalSection(&cs);
  SetEvent(evReq);
  if (!timeout) return 1;
  for (;;) {
    el = GetTickCount() - t0;
    if (el >= timeout || WaitForSingleObject(evResp, timeout - el) != WAIT_OBJECT_0) return 0;
    EnterCriticalSection(&cs);
    if (doneSeq == seq) {
      for (j = 0; j < n; j++) {
        r = qres[j];
        d = &q[map[j]].res;
        if (r == 1) *d = 1;
        else if (r == 0 && *d != 1) *d = 0;
      }
      LeaveCriticalSection(&cs);
      return 1;
    }
    LeaveCriticalSection(&cs);
  }
}

static int spell_batch(DICTQ* q, int nq) { return spell_ask(q, nq, 50); }

/* Load every layout's dictionary in the background so the first real word is fast. */
static void spell_warm(void) {
  DICTQ q[MAXL];
  int g;
  for (g = 0; g < g_nlay; g++) {
    q[g].g = g;
    q[g].w = L"hello";
    q[g].n = 5;
    q[g].typo = 0;
  }
  spell_ask(q, g_nlay, 0);
}

/* ---------- settings & word lists ---------- */

WCHAR ign[MAXIGN][MAXW + 1];
int   nign;
WCHAR apps[MAXAPPS][MAXW + 1];
int   napps;
WCHAR lastAppExe[MAXW + 1];
static WCHAR regbuf[MAXIGN * (MAXW + 1) + 2];

DWORD reg_dword(const WCHAR* name, DWORD def) {
  DWORD v, sz = sizeof v;
  return RegGetValueW(HKEY_CURRENT_USER, REGKEY, name, RRF_RT_REG_DWORD, 0, &v, &sz) ? def : v;
}
void reg_set_dword(const WCHAR* name, DWORD v) {
  RegSetKeyValueW(HKEY_CURRENT_USER, REGKEY, name, REG_DWORD, &v, sizeof v);
}
int reg_str(const WCHAR* name, WCHAR* out, int max) {
  DWORD sz = (DWORD)(max * sizeof(WCHAR));
  out[0] = 0;
  return !RegGetValueW(HKEY_CURRENT_USER, REGKEY, name, RRF_RT_REG_SZ, 0, out, &sz) && out[0];
}
void reg_set_str(const WCHAR* name, const WCHAR* s) {
  RegSetKeyValueW(HKEY_CURRENT_USER, REGKEY, name, REG_SZ, s, (DWORD)((wlen(s) + 1) * sizeof(WCHAR)));
}

void list_save(const WCHAR* name, WCHAR (*list)[MAXW + 1], int n) {
  int i, len = 0;
  for (i = 0; i < n; i++) {
    wcopy(regbuf + len, list[i], MAXW + 1);
    len += wlen(list[i]) + 1;
  }
  regbuf[len++] = 0;
  if (len == 1) regbuf[len++] = 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, REGKEY, name, REG_MULTI_SZ, regbuf, (DWORD)(len * sizeof(WCHAR)));
}

int list_load(const WCHAR* name, WCHAR (*list)[MAXW + 1], int max) {
  DWORD sz = sizeof regbuf - 2 * sizeof(WCHAR);
  WCHAR* p = regbuf;
  int n = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, REGKEY, name, RRF_RT_REG_MULTI_SZ, 0, regbuf, &sz)) return 0;
  regbuf[sz / sizeof(WCHAR)] = 0;
  regbuf[sz / sizeof(WCHAR) + 1] = 0;
  while (*p && n < max) {
    wcopy(list[n++], p, MAXW + 1);
    p += wlen(p) + 1;
  }
  return n;
}

int list_find(WCHAR (*list)[MAXW + 1], int n, const WCHAR* s) {
  int i;
  for (i = 0; i < n; i++)
    if (weq(list[i], s)) return i;
  return -1;
}

void list_remove(WCHAR (*list)[MAXW + 1], int* n, int at) {
  int i;
  for (i = at + 1; i < *n; i++) wcopy(list[i - 1], list[i], MAXW + 1);
  (*n)--;
}

/* A user-typed word, cleaned: trimmed, lower-case, no inner spaces. */
int clean_word(const WCHAR* in, WCHAR* out) {
  int i, n = 0;
  while (*in == L' ' || *in == L'\t') in++;
  for (i = 0; in[i] && n < MAXW; i++) out[n++] = eng_lower(in[i]);
  while (n && (out[n - 1] == L' ' || out[n - 1] == L'\t')) n--;
  out[n] = 0;
  for (i = 0; i < n; i++)
    if (out[i] == L' ' || out[i] == L'\t') return 0;
  return n > 0 && !in[i];
}

/* The never-change key is the word as it looked when typed, lower-cased. */
static int ign_key(const KEY* k, int n, int g, WCHAR* s) {
  int i;
  if (!eng_render(g, k, n, s, 0)) return 0;
  for (i = 0; i < n; i++) s[i] = eng_lower(s[i]);
  return 1;
}

static int ign_has(const KEY* k, int n, int g) {
  WCHAR s[MAXW + 1];
  return nign && ign_key(k, n, g, s) && list_find(ign, nign, s) >= 0;
}

static void ign_add(const KEY* k, int n, int g) {
  WCHAR s[MAXW + 1];
  if (!ign_key(k, n, g, s) || list_find(ign, nign, s) >= 0) return;
  if (nign == MAXIGN) list_remove(ign, &nign, 0);   /* forget the oldest */
  wcopy(ign[nign++], s, MAXW + 1);
  list_save(L"Ignore", ign, nign);
}

void apply_engine_settings(void) {
  g_par.minGain = g_par.minGainShort = sensGain[setSens < 3 ? setSens : 1];
}

/* Layouts the user switched off are kept as the low 32 bits of their HKLs. */
static void layouts_apply(void) {
  static WCHAR off[32][MAXW + 1];
  WCHAR hex[16];
  int n = list_load(L"Disabled", off, 32), g, i;
  for (g = 0; g < g_nlay; g++) {
    g_lay[g].enabled = 1;
    for (i = 0; i < g_lay[g].nhkl; i++) {
      wsprintfW(hex, L"%08lx", (DWORD)(UINT_PTR)g_lay[g].hkl[i]);
      if (list_find(off, n, hex) >= 0) g_lay[g].enabled = 0;
    }
  }
}

void layouts_save(void) {
  static WCHAR off[32][MAXW + 1];
  int n = 0, g, i;
  for (g = 0; g < g_nlay; g++)
    if (!g_lay[g].enabled)
      for (i = 0; i < g_lay[g].nhkl && n < 32; i++)
        wsprintfW(off[n++], L"%08lx", (DWORD)(UINT_PTR)g_lay[g].hkl[i]);
  list_save(L"Disabled", off, n);
}

static void settings_load(void) {
  int i, n;
  enabled = reg_dword(L"Enabled", 1) != 0;
  setPause = reg_dword(L"PauseKey", 1);
  setShifts = reg_dword(L"ShiftKeys", 1);
  setConvertHk = reg_dword(L"ConvertHotkey", 0);
  setToggleHk = reg_dword(L"ToggleHotkey", 0);
  setSelPause = reg_dword(L"SelPause", 1);
  setSelHk = reg_dword(L"SelHotkey", 0);
  setSens = reg_dword(L"Sensitivity", 1);
  setHints = reg_dword(L"Hints", 1);
  setEnter = reg_dword(L"EnterKey", 0);
  setUpdates = reg_dword(L"Updates", 1);
  g_par.slang = reg_dword(L"Slang", 1) != 0;
  nign = list_load(L"Ignore", ign, MAXIGN);
  napps = list_load(L"Apps", apps, MAXAPPS);
  n = list_load(L"MyWords", g_user, MAXUSER);
  for (g_nuser = 0, i = 0; i < n; i++)
    if (clean_word(g_user[i], g_user[g_nuser])) g_nuser++;
  apply_engine_settings();
  ai_load_settings();
}

/* ---------- autostart & Start menu ---------- */

BOOL autostart_get(void) {
  DWORD sz = 0;
  return RegGetValueW(HKEY_CURRENT_USER, RUNKEY, APP, RRF_RT_REG_SZ, 0, 0, &sz) == ERROR_SUCCESS;
}

void autostart_set(BOOL on) {
  WCHAR path[MAX_PATH + 16];
  DWORD n;
  if (!on) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, RUNKEY, APP);
    return;
  }
  path[0] = L'"';
  n = GetModuleFileNameW(0, path + 1, MAX_PATH);
  path[n + 1] = 0;
  wcat(path, L"\" --tray", MAX_PATH + 16);
  RegSetKeyValueW(HKEY_CURRENT_USER, RUNKEY, APP, REG_SZ, path, (wlen(path) + 1) * sizeof(WCHAR));
}

static BOOL shortcut_path(WCHAR* out) {
  PWSTR p;
  if (FAILED(SHGetKnownFolderPath(&FOLDERID_Programs_, 0, 0, &p))) return FALSE;
  wcopy(out, p, MAX_PATH);
  CoTaskMemFree(p);
  wcat(out, L"\\LangFix.lnk", MAX_PATH);
  return TRUE;
}

BOOL shortcut_get(void) {
  WCHAR path[MAX_PATH];
  return shortcut_path(path) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void shortcut_set(BOOL on) {
  WCHAR path[MAX_PATH], exe[MAX_PATH];
  IShellLinkW* sl = 0;
  IPersistFile* pf = 0;
  if (!shortcut_path(path)) return;
  if (!on) {
    DeleteFileW(path);
    return;
  }
  GetModuleFileNameW(0, exe, MAX_PATH);
  if (FAILED(CoCreateInstance(&CLSID_ShellLink_, 0, CLSCTX_INPROC_SERVER, &IID_IShellLinkW_, (void**)&sl))) return;
  IShellLinkW_SetPath(sl, exe);
  IShellLinkW_SetDescription(sl, L"Fixes words typed in the wrong keyboard layout");
  IShellLinkW_SetIconLocation(sl, exe, 0);
  if (SUCCEEDED(IShellLinkW_QueryInterface(sl, &IID_IPersistFile_, (void**)&pf))) {
    IPersistFile_Save(pf, path, TRUE);
    IPersistFile_Release(pf);
  }
  IShellLinkW_Release(sl);
}

/* ---------- input injection ---------- */

static INPUT inj[MAXW * 8 + 16];
static int   ninj;
static WORD  batch;                 /* id of the newest injected batch */
static int   pending;               /* its events not yet seen by the hook */
static int   holding;               /* user keys are being held back */
static DWORD batchTick;
static INPUT stale[MAXHELD], fresh[MAXHELD], flushbuf[MAXHELD * 2];
static int   nstale, nfresh;
static BYTE  relVk;                 /* modifier key-up to send before the next fix */
static int   relExt;

static void inj_begin(void) {
  ninj = 0;
  batch = (WORD)((batch + 1) & 0x7FFF);
}

static void inj_key(WORD vk, WORD scan, DWORD flags) {
  INPUT* i = &inj[ninj++];
  i->type = INPUT_KEYBOARD;
  i->ki.wVk = vk;
  i->ki.wScan = scan;
  i->ki.dwFlags = flags;
  i->ki.time = 0;
  i->ki.dwExtraInfo = TAG | batch;
}

static void inj_tap(WORD vk, WORD scan) {
  inj_key(vk, scan, 0);
  inj_key(vk, scan, KEYEVENTF_KEYUP);
}

static void inj_text(const WCHAR* t, int n) {
  int i;
  for (i = 0; i < n; i++) {
    inj_key(0, t[i], KEYEVENTF_UNICODE);
    inj_key(0, t[i], KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
  }
}

/* Windows usually runs injected keys through the hook synchronously, nested inside
   SendInput and ahead of keys already queued; set the counters first so both that and
   the asynchronous case work. Callers must finish their own state updates before this. */
static void inj_send(INPUT* in, int n) {
  UINT sent;
  if (!n) return;
  pending = n;
  holding = 1;
  batchTick = GetTickCount();
  sent = SendInput((UINT)n, in, sizeof(INPUT));
  pending -= n - (int)sent;            /* never inserted (UIPI): will never arrive */
  if (pending <= 0) pending = holding = 0;
}

static void inj_mask(void) {
  inj_begin();
  inj_tap(VK_MASK, 0);
  inj_send(inj, ninj);
}

static void hold(const KBDLLHOOKSTRUCT* k, int up, INPUT* q, int* nq) {
  INPUT* i;
  if (*nq >= MAXHELD) return;
  i = &q[(*nq)++];
  i->type = INPUT_KEYBOARD;
  if (k->vkCode == VK_PACKET) {
    i->ki.wVk = 0;
    i->ki.dwFlags = KEYEVENTF_UNICODE;
  } else {
    i->ki.wVk = (WORD)k->vkCode;
    i->ki.dwFlags = (k->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
  }
  i->ki.wScan = (WORD)k->scanCode;
  if (up) i->ki.dwFlags |= KEYEVENTF_KEYUP;
  i->ki.time = 0;
}

/* Replay held keys: leftovers of an interrupted replay first, then newer keys. */
static void flush_held(void) {
  int i, n = 0;
  if (!nstale && !nfresh) return;
  batch = (WORD)((batch + 1) & 0x7FFF);
  for (i = 0; i < nstale; i++) flushbuf[n++] = stale[i];
  for (i = 0; i < nfresh; i++) flushbuf[n++] = fresh[i];
  for (i = 0; i < n; i++) flushbuf[i].ki.dwExtraInfo = TAG | TAG_REPLAY | batch;
  nstale = nfresh = 0;
  inj_send(flushbuf, n);
}

static int   forcedG = -1;          /* layout we just requested, until the window reports it */
static HWND  forcedWnd;
static DWORD forcedTick;

void switch_layout_in(HWND wnd, int g) {
  HKL h;
  if (!wnd || g < 0 || g >= g_nlay) return;
  h = g_lay[g].recent ? g_lay[g].recent : g_lay[g].hkl[0];
  PostMessageW(wnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)h);
  forcedG = g;
  forcedWnd = wnd;
  forcedTick = GetTickCount();
}

/* Erase `erase` chars, type `t`, optionally a space, and switch the layout to g. */
static void replace_text(int erase, const WCHAR* t, int n, int space, int g) {
  int i;
  inj_begin();
  if (relVk) {                       /* release the hotkey's modifier first */
    inj_key(relVk, (WORD)MapVirtualKeyW(relVk, MAPVK_VK_TO_VSC), KEYEVENTF_KEYUP | (relExt ? KEYEVENTF_EXTENDEDKEY : 0));
    relVk = 0;
  }
  for (i = 0; i < erase; i++) inj_tap(VK_BACK, 0x0E);
  inj_text(t, n);
  if (space) inj_tap(VK_SPACE, 0x39);
  switch_layout_in(GetForegroundWindow(), g);
  inj_send(inj, ninj);
}

/* ---------- word tracking ---------- */

static KEY  tok[MAXW];
static int  ntok, tokShown, tokBad, tokManual;
static HWND tokWnd;
static int  fgExcluded;             /* the foreground app is on the Apps list */
static KEY  lastK[MAXW];            /* the word before the current one */
static int  lastN, lastShown, lastOrig, lastManual;
static int  lastValid;              /* last word + one space + current word, nothing else */
static int  ctxG = -1;              /* layout the previous word was confidently in */
static DWORD ctxTick;
static BYTE swallowUp[256];
static int  caps, capsHeld, lsh, rsh;
static int  pendingAct;             /* custom convert hotkey waiting for its modifiers to be released */

static const BYTE MODKEYS[] = { VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN };

void reset_all(void) {
  ntok = tokBad = tokManual = 0;
  lastValid = 0;
}

int group_of_window(HWND wnd) {
  HKL h = GetKeyboardLayout(GetWindowThreadProcessId(wnd, 0));
  int g;
  if (!h) return -1;
  g = eng_find(h);
  if (g < 0 && (g = eng_add_hkl(h)) >= 0) spell_warm();
  return g;
}

static int cur_group(HWND fg) {
  int g = group_of_window(fg);
  if (g >= 0 && forcedG >= 0) {
    if (g == forcedG || fg != forcedWnd || GetTickCount() - forcedTick > 400) forcedG = -1;
    else g = forcedG;               /* the window has not processed our switch yet */
  }
  return g;
}

int exe_of_window(HWND wnd, WCHAR* out, int max) {
  WCHAR path[MAX_PATH];
  DWORD pid = 0, n = MAX_PATH;
  HANDLE p;
  int i, start = 0;
  out[0] = 0;
  GetWindowThreadProcessId(wnd, &pid);
  if (!pid || !(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return 0;
  if (QueryFullProcessImageNameW(p, 0, path, &n)) {
    for (i = 0; path[i]; i++)
      if (path[i] == L'\\') start = i + 1;
    for (i = 0; path[start + i] && i < max - 1; i++) out[i] = eng_lower(path[start + i]);
    out[i] = 0;
  }
  CloseHandle(p);
  return out[0] != 0;
}

static int is_password_field(HWND fg) {
  GUITHREADINFO gi;
  WCHAR cls[16];
  gi.cbSize = sizeof gi;
  if (!GetGUIThreadInfo(GetWindowThreadProcessId(fg, 0), &gi) || !gi.hwndFocus) return 0;
  if (!GetClassNameW(gi.hwndFocus, cls, 16) || CompareStringOrdinal(cls, -1, L"Edit", -1, TRUE) != CSTR_EQUAL) return 0;
  return (GetWindowLongW(gi.hwndFocus, GWL_STYLE) & ES_PASSWORD) != 0;
}

static void save_last(int shown, int orig, int manual) {
  int i;
  for (i = 0; i < ntok; i++) lastK[i] = tok[i];
  lastN = ntok;
  lastShown = shown;
  lastOrig = orig;
  lastManual = manual;
  lastValid = 1;
}

/* "ghbdtn → привет   ·   Pause undoes" for the hint near the caret. */
static void make_hint(int cur, const WCHAR* fixed) {
  WCHAR typed[MAXW + 1];
  const WCHAR* undo = setPause ? L"Pause" : setShifts ? L"Shift+Shift" : setConvertHk ? L"your hotkey" : 0;
  if (!setHints || !eng_render(cur, tok, ntok, typed, 0)) return;
  wsprintfW(g_hint, L"%s  \x2192  %s", typed, fixed);
  if (undo) {
    wcat(g_hint, L"     \x00B7  ", 256);
    wcat(g_hint, undo, 256);
    wcat(g_hint, L" undoes", 256);
  }
  PostMessageW(hWnd, WM_HINT, 0, 0);
}

/* A word ended with Space (enter = 0) or Enter (enter = 1). Returns 1 to swallow the key. */
static int on_boundary(HWND fg, int enter) {
  WCHAR out[MAXW + 1], prev[MAXW * 2 + 3];
  int cur, t, t2, i, n, ctx, erase, len;
  if (!ntok || tokBad) {               /* double space or unknown text */
    lastValid = 0;
    ntok = tokBad = tokManual = 0;
    return 0;
  }
  cur = cur_group(fg);
  ctx = ctxG >= 0 && GetTickCount() - ctxTick < CTX_MS ? ctxG : -1;
  if (!tokManual && !fgExcluded && cur == tokShown && !is_password_field(fg)) {
    t = eng_decide(tok, ntok, cur, ctx, spell_batch, out);
    if (t >= 0 && !ign_has(tok, ntok, cur)) {
      n = 0;
      /* The word before may have been ambiguous ("to" reads as Hebrew "אם"):
         if it also reads better in the new layout, fix it together with this one. */
      if (lastValid && lastOrig < 0 && !lastManual && lastShown == cur &&
          eng_gain(lastK, lastN, cur, spell_batch, &t2) > 0 && t2 == t &&
          eng_render(t, lastK, lastN, prev, 1)) {
        n = lastN;
        prev[n++] = L' ';
      }
      for (i = 0; i < ntok; i++) prev[n + i] = out[i];
      erase = ntok + (n ? lastN + 1 : 0);
      len = n + ntok;
      make_hint(cur, out);
      if (!enter) swallowUp[VK_SPACE] = 1;
      save_last(t, cur, 0);
      if (enter) lastValid = 0;        /* a line break follows: undo can't reach it */
      ctxG = t;
      ctxTick = GetTickCount();
      ntok = tokBad = tokManual = 0;
      fixCount++;
      PostMessageW(hWnd, WM_STATS, 0, 0);
      replace_text(erase, prev, len, !enter, t);   /* last: the hook may run again inside it */
      return !enter;                   /* Enter itself goes through after the fixed word */
    } else {
      save_last(cur, -1, 0);
      ctxG = g_gain < 0 ? cur : -1;   /* only a word that clearly belongs here is context */
      ctxTick = GetTickCount();
    }
  } else if (cur == tokShown) {
    save_last(tokShown, -1, tokManual);
  } else {
    lastValid = 0;
  }
  ntok = tokBad = tokManual = 0;
  return 0;
}

/* Convert the word being typed, or the word just finished (undoing an automatic fix
   and remembering it). Returns 1 if text was replaced. */
static int hotkey(void) {
  WCHAR out[MAXW + 1];
  int t;
  if (ntok > 0) {
    if (tokBad || (t = eng_pick(tok, ntok, tokShown, spell_batch, out)) < 0) return 0;
    tokShown = t;
    tokManual = 1;
    convCount++;
    PostMessageW(hWnd, WM_STATS, 0, 0);
    replace_text(ntok, out, ntok, 0, t);
    return 1;
  }
  if (!lastValid) return 0;
  if (lastOrig >= 0) {
    t = lastOrig;
    if (!eng_render(t, lastK, lastN, out, 0)) return 0;
    ign_add(lastK, lastN, t);
    undoCount++;
  } else if ((t = eng_pick(lastK, lastN, lastShown, spell_batch, out)) < 0) {
    return 0;
  } else {
    convCount++;
  }
  lastShown = t;
  lastOrig = -1;
  lastManual = 1;
  ctxG = t;
  ctxTick = GetTickCount();
  PostMessageW(hWnd, WM_STATS, 0, 0);
  replace_text(lastN + 1, out, lastN, 1, t);
  return 1;
}

static void on_back(void) {
  int i;
  if (ntok > 0) {
    if (!--ntok) tokBad = tokManual = 0;
  } else if (lastValid) {            /* the space after the last word is gone: back inside it */
    for (i = 0; i < lastN; i++) tok[i] = lastK[i];
    ntok = lastN;
    tokShown = lastShown;
    tokBad = 0;
    tokManual = lastManual || lastOrig >= 0;
    lastValid = 0;
  }
}

static int is_modkey(DWORD vk) {
  int i;
  for (i = 0; i < (int)sizeof MODKEYS; i++)
    if (MODKEYS[i] == vk) return 1;
  return 0;
}

static int other_mods_held(DWORD vk) {
  int i;
  for (i = 0; i < (int)sizeof MODKEYS; i++)
    if (MODKEYS[i] != vk && GetAsyncKeyState(MODKEYS[i]) < 0) return 1;
  return 0;
}

/* HOTKEYF_* of the modifiers held now, -1 if a Windows key is held. */
static int hk_mods(void) {
  if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) < 0) return -1;
  return (GetAsyncKeyState(VK_SHIFT) < 0 ? HOTKEYF_SHIFT : 0) |
         (GetAsyncKeyState(VK_CONTROL) < 0 ? HOTKEYF_CONTROL : 0) |
         (GetAsyncKeyState(VK_MENU) < 0 ? HOTKEYF_ALT : 0);
}

static int hk_match(DWORD hk, DWORD vk, int mods) {
  return hk && LOBYTE(hk) == vk && (HIBYTE(hk) & 7) == mods;
}

static int own_window(HWND fg) {
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

/* The hotkey bound to this key press, 0 if none. */
static int which_hotkey(DWORD vk, int mods) {
  if (mods < 0) return 0;
  if (hk_match(setToggleHk, vk, mods)) return ACT_TOGGLE;
  if (aiEnabled && hk_match(aiFixHk, vk, mods)) return ACT_AIFIX;
  if (aiEnabled && hk_match(aiMenuHk, vk, mods)) return ACT_AIMENU;
  if (hk_match(setSelHk, vk, mods) || (setSelPause && vk == VK_PAUSE && mods == HOTKEYF_SHIFT)) return ACT_SELCONV;
  if (enabled && hk_match(setConvertHk, vk, mods)) return ACT_CONVERT;
  return 0;
}

/* Returns 1 to swallow the key. */
static int handle(const KBDLLHOOKSTRUCT* k, int up) {
  DWORD vk = k->vkCode;
  HWND fg;
  int cur, mods, act, *me, other;

  if (vk > 255) return 0;
  if (up) {
    if (vk == VK_LSHIFT) lsh = 0;
    else if (vk == VK_RSHIFT) rsh = 0;
    else if (vk == VK_CAPITAL) capsHeld = 0;
    if (pendingAct && is_modkey(vk) && !other_mods_held(vk)) {
      pendingAct = 0;                  /* the custom hotkey's last modifier is released */
      relVk = (BYTE)vk;
      relExt = (k->flags & LLKHF_EXTENDED) != 0;
      if (hotkey()) return 1;          /* our batch released the modifier itself */
      relVk = 0;
    }
    if (swallowUp[vk]) { swallowUp[vk] = 0; return 1; }
    return 0;
  }
  switch (vk) {
  case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
    me = vk == VK_RSHIFT ? &rsh : &lsh;
    other = vk == VK_RSHIFT ? lsh : rsh;
    if (!*me && other && enabled && setShifts) hotkey();
    *me = 1;
    return 0;
  case VK_CAPITAL:
    if (!capsHeld) caps ^= 1;
    capsHeld = 1;
    return 0;
  case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
  case VK_LMENU: case VK_RMENU: case VK_MENU: case VK_LWIN: case VK_RWIN:
    return 0;
  case VK_ESCAPE:
    if (ai_busy()) { ai_cancel(); swallowUp[VK_ESCAPE] = 1; return 1; }
    break;
  }

  fg = GetForegroundWindow();
  if (own_window(fg)) return 0;       /* our own windows: typing there is never fixed */

  mods = hk_mods();
  if ((act = which_hotkey(vk, mods)) != 0) {
    swallowUp[vk] = 1;
    if (mods & HOTKEYF_ALT) inj_mask();
    if (act == ACT_TOGGLE) PostMessageW(hWnd, WM_TOGGLE, 0, 0);
    else if (act != ACT_CONVERT) { reset_all(); PostMessageW(hWnd, WM_SEL, (WPARAM)act, 0); }
    else if (mods & (HOTKEYF_CONTROL | HOTKEYF_ALT)) pendingAct = 1;  /* fix once they are released */
    else hotkey();
    return 1;
  }
  if (!enabled) return 0;

  if (fg != tokWnd) {
    reset_all();
    ctxG = -1;
    tokWnd = fg;
    fgExcluded = 0;
    if (exe_of_window(fg, lastAppExe, MAXW + 1)) fgExcluded = list_find(apps, napps, lastAppExe) >= 0;
  }
  if (mods != 0 && mods != HOTKEYF_SHIFT) {
    reset_all();                       /* shortcuts edit text in ways we cannot follow */
    return 0;
  }
  if (vk == VK_PAUSE && setPause && !mods) {
    if (!hotkey()) return 0;
    swallowUp[VK_PAUSE] = 1;
    return 1;
  }
  if (vk == VK_BACK) { on_back(); return 0; }
  if (vk == VK_SPACE) return on_boundary(fg, 0);
  if (vk == VK_RETURN) {
    if (setEnter && ntok) on_boundary(fg, 1);
    reset_all();
    return 0;
  }

  cur = cur_group(fg);
  mods = (mods & HOTKEYF_SHIFT ? 1 : 0) | (caps ? 2 : 0);
  if (cur < 0 || !g_lay[cur].map[vk][mods]) {   /* Tab, arrows, F-keys... */
    reset_all();
    return 0;
  }
  if (!ntok) {
    tokShown = cur;
    tokBad = tokManual = 0;
  } else if (cur != tokShown) {
    tokBad = 1;                        /* layout changed inside the word */
  }
  if (g_lay[cur].bad[vk]) tokBad = 1;
  if (ntok < MAXW) {
    tok[ntok].vk = (BYTE)vk;
    tok[ntok].mods = (BYTE)mods;
    ntok++;
  } else {
    tokBad = 1;
  }
  return 0;
}

#ifdef LF_DEBUG
static HANDLE dbgFile;
static void dbg(const KBDLLHOOKSTRUCT* k, int up, const char* what) {
  char line[160];
  DWORD n;
  if (!dbgFile) dbgFile = CreateFileW(L"langfix-debug.log", GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
  n = (DWORD)wsprintfA(line, "%lu vk=%02x sc=%04x %s ex=%08lx batch=%u pend=%d hold=%d ntok=%d %s\r\n",
                       GetTickCount(), k->vkCode, k->scanCode, up ? "UP  " : "DOWN", (unsigned long)k->dwExtraInfo,
                       batch, pending, holding, ntok, what);
  WriteFile(dbgFile, line, n, &n, 0);
}
#define DBG(w) dbg(k, up, w)
#else
#define DBG(w)
#endif

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
  KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
  int up, r = 0;
  if (code != HC_ACTION) return CallNextHookEx(0, code, wp, lp);
  up = (k->flags & LLKHF_UP) != 0;
  lastHookTime = k->time;
  DBG("in");

  if ((k->dwExtraInfo & TAG_MASK) == TAG_AI) {          /* our copy/paste for selected text */
    reset_all();
    return CallNextHookEx(0, code, wp, lp);
  }
  if ((k->dwExtraInfo & TAG_MASK) == TAG) {              /* our own input */
    if ((WORD)(k->dwExtraInfo & 0x7FFF) != batch) {     /* replay overtaken by a new fix */
      hold(k, up, stale, &nstale);
      return 1;
    }
    if (pending > 0 && --pending == 0) holding = 0;
    if (k->dwExtraInfo & TAG_REPLAY) r = handle(k, up);
    if (!holding) flush_held();
    return r ? 1 : CallNextHookEx(0, code, wp, lp);
  }
  if (holding && GetTickCount() - batchTick > 300) {   /* injected input got lost */
    holding = pending = 0;
    flush_held();
  }
  if (holding) {
    hold(k, up, fresh, &nfresh);
    return 1;
  }
  if (handle(k, up)) return 1;
  return CallNextHookEx(0, code, wp, lp);
}

static LRESULT CALLBACK MsProc(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION) {
    lastHookTime = ((MSLLHOOKSTRUCT*)lp)->time;
    if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN || wp == WM_XBUTTONDOWN)
      reset_all();                     /* the caret may have moved */
  }
  return CallNextHookEx(0, code, wp, lp);
}

static void hooks_install(void) {
  if (hkKb) UnhookWindowsHookEx(hkKb);
  if (hkMs) UnhookWindowsHookEx(hkMs);
  hkKb = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, hInst, 0);
  hkMs = SetWindowsHookExW(WH_MOUSE_LL, MsProc, hInst, 0);
  hookInstallTick = lastHookTime = GetTickCount();
}

/* ---------- tray ---------- */

static void tray(DWORD msg) {
  nid.cbSize = sizeof nid;
  nid.hWnd = hWnd;
  nid.uID = 1;
  nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
  nid.uCallbackMessage = WM_TRAY;
  nid.hIcon = enabled ? icoOn : icoOff;
  wcopy(nid.szTip, enabled ? L"LangFix - on" : L"LangFix - paused", 128);
  Shell_NotifyIconW(msg, &nid);
  if (msg == NIM_ADD) {
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }
}

void balloon(const WCHAR* title, const WCHAR* text) {
  nid.uFlags = NIF_INFO;
  wcopy(nid.szInfoTitle, title, 64);
  wcopy(nid.szInfo, text, 256);
  nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void set_enabled(BOOL on) {
  enabled = on;
  reset_all();
  reg_set_dword(L"Enabled", on);
  tray(NIM_MODIFY);
  ui_stats();
}

static void show_menu(int x, int y) {
  HMENU m = CreatePopupMenu();
  int cmd;
  AppendMenuW(m, MF_STRING, IDM_OPEN, L"&Open LangFix");
  SetMenuDefaultItem(m, IDM_OPEN, FALSE);
  AppendMenuW(m, MF_SEPARATOR, 0, 0);
  AppendMenuW(m, MF_STRING | (enabled ? MF_CHECKED : 0), IDM_ENABLE, L"&Enabled");
  AppendMenuW(m, MF_STRING | (autostart_get() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with &Windows");
  AppendMenuW(m, MF_SEPARATOR, 0, 0);
  AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
  SetForegroundWindow(hWnd);
  cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, 0, hWnd, 0);
  PostMessageW(hWnd, WM_NULL, 0, 0);
  DestroyMenu(m);
  switch (cmd) {
  case IDM_OPEN: ui_show(-1); break;
  case IDM_ENABLE: set_enabled(!enabled); break;
  case IDM_AUTOSTART: autostart_set(!autostart_get()); ui_stats(); break;
  case IDM_EXIT:
    if (hDlg) DestroyWindow(hDlg);
    DestroyWindow(hWnd);
    break;
  }
}

static void on_timer(void) {
  LASTINPUTINFO li;
  if (eng_layouts_changed()) {
    eng_init_layouts();
    layouts_apply();
    reset_all();
    ctxG = forcedG = -1;
    spell_warm();
    ui_langs_changed();
  }
  /* Windows silently drops hooks that ever time out; if input arrives that we never
     saw, put them back (rate-limited, harmless when input went to an elevated window). */
  li.cbSize = sizeof li;
  if (GetLastInputInfo(&li) && (int)(li.dwTime - lastHookTime) > 1000 &&
      GetTickCount() - hookInstallTick > 30000)
    hooks_install();
  if (setUpdates) update_check(FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == wmTaskbarCreated && m) { tray(NIM_ADD); return 0; }
  switch (m) {
  case WM_TRAY:
    switch (LOWORD(l)) {
    case NIN_SELECT: case NIN_KEYSELECT: ui_show(-1); break;
    case NIN_BALLOONUSERCLICK: update_open(); break;
    case WM_CONTEXTMENU: show_menu((short)LOWORD(w), (short)HIWORD(w)); break;
    }
    return 0;
  case WM_SHOWUI:
    ui_show((int)w);
    return 0;
  case WM_TOGGLE:
    set_enabled(!enabled);
    hint_show(enabled ? L"LangFix is on" : L"LangFix is paused", 1500);
    return 0;
  case WM_STATS:
    ui_stats();
    return 0;
  case WM_HINT:
    hint_show(g_hint, 2400);
    return 0;
  case WM_SEL: case WM_SEL_DONE: case WM_AI_DONE: case WM_JOB_END: case WM_UPDATE:
    return ai_message(m, w, l);
  case WM_TIMER:
    if (w == TIMER_HINT) hint_hide();
    else on_timer();
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

static int has_arg(const WCHAR* arg) {
  const WCHAR* c = GetCommandLineW();
  int n = wlen(arg), i;
  for (; *c; c++) {
    for (i = 0; i < n && c[i] == arg[i]; i++) {}
    if (i == n) return 1;
  }
  return 0;
}

static int run(void) {
  INITCOMMONCONTROLSEX icc;
  WNDCLASSW wc;
  MSG msg;
  HWND other;
  int tray_only = has_arg(L"--tray");

  CreateMutexW(0, TRUE, L"Local\\LangFix.Instance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {        /* already running: just show it */
    other = FindWindowW(L"LangFixWnd", 0);
    if (other && !tray_only) {
      AllowSetForegroundWindow(ASFW_ANY);
      PostMessageW(other, WM_SHOWUI, (WPARAM)-1, 0);
    }
    return 0;
  }
  hInst = GetModuleHandleW(0);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  CoInitializeEx(0, COINIT_APARTMENTTHREADED);         /* Start menu shortcut */
  icc.dwSize = sizeof icc;
  icc.dwICC = ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
  InitCommonControlsEx(&icc);

  settings_load();
  caps = GetKeyState(VK_CAPITAL) & 1;
  eng_init_layouts();
  layouts_apply();

  InitializeCriticalSection(&cs);
  evReq = CreateEventW(0, FALSE, FALSE, 0);
  evResp = CreateEventW(0, FALSE, FALSE, 0);
  CloseHandle(CreateThread(0, 256 * 1024, spell_thread, 0, 0, 0));
  spell_warm();

  /* the exe may have moved since autostart / the shortcut were made */
  if (autostart_get()) autostart_set(TRUE);
  if (shortcut_get()) shortcut_set(TRUE);

  wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  icoOn = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  icoOff = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_OFF), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
  icoBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_ON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
  zero(&wc, sizeof wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"LangFixWnd";
  RegisterClassW(&wc);
  hWnd = CreateWindowExW(0, wc.lpszClassName, APP, WS_POPUP, 0, 0, 0, 0, 0, 0, hInst, 0);
  tray(NIM_ADD);

  hooks_install();
  SetTimer(hWnd, TIMER_MAIN, 5000, 0);
  if (!tray_only) ui_show(-1);
  if (setUpdates) update_check(FALSE);
  while (GetMessageW(&msg, 0, 0, 0) > 0) {
    if (hDlg && IsDialogMessageW(hDlg, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  UnhookWindowsHookEx(hkKb);
  UnhookWindowsHookEx(hkMs);
  Shell_NotifyIconW(NIM_DELETE, &nid);
  return 0;
}

void WINAPI start(void) { ExitProcess((UINT)run()); }
