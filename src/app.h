/* LangFix - declarations shared by main.c (hook, typing, tray), ui.c (settings window)
   and ai.c (selected-text actions: layout conversion, Claude writing tools, hints, updates). */
#pragma once
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include "engine.h"
#include "resource.h"

#define APP          L"LangFix"
#define APP_VERSION  L"1.3.0"
#define REPO_URL     L"https://github.com/alexeybe1kin/LangFix"
#define REGKEY       L"Software\\LangFix"

#define WM_TRAY      (WM_APP + 1)
#define WM_SHOWUI    (WM_APP + 2)        /* also sent by a second LangFix.exe; wParam = page */
#define WM_TOGGLE    (WM_APP + 3)
#define WM_STATS     (WM_APP + 4)
#define WM_HINT      (WM_APP + 5)        /* show g_hint near the caret */
#define WM_SEL       (WM_APP + 6)        /* wParam = ACT_* selected-text action */
#define WM_SEL_DONE  (WM_APP + 7)        /* selection captured */
#define WM_AI_DONE   (WM_APP + 8)        /* lParam = AIREQ* */
#define WM_JOB_END   (WM_APP + 9)        /* paste finished */
#define WM_UPDATE    (WM_APP + 10)       /* lParam = heap string with the new version */
#define WM_MOUSEHOOK (WM_APP + 11)       /* install or remove the mouse hook as needed */

#define TIMER_MAIN   1
#define TIMER_HINT   2

#define TAG_AI       0x4C410000u         /* dwExtraInfo of Ctrl+C / Ctrl+V we send ('LA') */
#define MAXIGN       256
#define MAXAPPS      64

enum { ACT_CONVERT = 1, ACT_TOGGLE, ACT_SELCONV, ACT_AIMENU, ACT_AIFIX };
enum { PAGE_GENERAL, PAGE_HOTKEYS, PAGE_LANGS, PAGE_WORDS, PAGE_AI, PAGE_APPS, NPAGES };

/* ---- main.c ---- */
extern HINSTANCE hInst;
extern HWND  hWnd;
extern HICON icoOn, icoOff, icoBig;
extern BOOL  enabled;
extern DWORD setPause, setShifts, setConvertHk, setToggleHk, setSens, setHints, setEnter, setUpdates,
             setSelPause, setSelHk;
extern int   fixCount, undoCount, convCount, aiCount;
extern WCHAR ign[MAXIGN][MAXW + 1];
extern int   nign;
extern WCHAR apps[MAXAPPS][MAXW + 1];
extern int   napps;
extern WCHAR lastAppExe[MAXW + 1];        /* the app typed in last, for the Apps tab */
extern WCHAR g_hint[256];

int   wlen(const WCHAR* s);
void  wcopy(WCHAR* d, const WCHAR* s, int max);
void  wcat(WCHAR* d, const WCHAR* s, int max);
int   weq(const WCHAR* a, const WCHAR* b);
void  zero(void* p, int n);
void* mem_alloc(SIZE_T n);
void  mem_free(void* p);

DWORD reg_dword(const WCHAR* name, DWORD def);
void  reg_set_dword(const WCHAR* name, DWORD v);
int   reg_str(const WCHAR* name, WCHAR* out, int max);
void  reg_set_str(const WCHAR* name, const WCHAR* s);
void  list_save(const WCHAR* name, WCHAR (*list)[MAXW + 1], int n);
int   list_load(const WCHAR* name, WCHAR (*list)[MAXW + 1], int max);
int   list_find(WCHAR (*list)[MAXW + 1], int n, const WCHAR* s);
void  list_remove(WCHAR (*list)[MAXW + 1], int* n, int at);
int   clean_word(const WCHAR* in, WCHAR* out);

void  apply_engine_settings(void);
void  layouts_save(void);
BOOL  autostart_get(void);
void  autostart_set(BOOL on);
BOOL  shortcut_get(void);
void  shortcut_set(BOOL on);
void  set_enabled(BOOL on);
void  balloon(const WCHAR* title, const WCHAR* text);
int   spell_ask(DICTQ* q, int nq, DWORD timeout);
void  reset_all(void);
void  switch_layout_in(HWND wnd, int g);
int   group_of_window(HWND wnd);
int   exe_of_window(HWND wnd, WCHAR* out, int max);

/* ---- ui.c ---- */
extern HWND hDlg;
void  ui_show(int page);
void  ui_stats(void);
void  ui_langs_changed(void);
void  ui_ai_result(const WCHAR* text);
int   ui_ask(HWND owner, WCHAR* instr, int max);
int   hk_valid(DWORD hk);

/* ---- ai.c ---- */
extern DWORD aiEnabled, aiModel, aiMenuHk, aiFixHk;
#define NMODELS 3
const WCHAR* ai_model_label(int i);
int   ai_key_get(WCHAR* out, int max);
void  ai_key_set(const WCHAR* key);
void  ai_load_settings(void);
int   ai_busy(void);
void  ai_cancel(void);
void  ai_test(void);
void  sel_start(int act);
LRESULT ai_message(UINT m, WPARAM w, LPARAM l);
void  hint_show(const WCHAR* text, DWORD ms);
void  hint_hide(void);
void  update_check(BOOL force);
void  update_open(void);
