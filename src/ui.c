/* LangFix - the settings window: a tab control with one child dialog per page.
   Every change is saved at once; closing the window keeps LangFix running in the tray. */
#include "app.h"
#include <uxtheme.h>

HWND hDlg;
static HWND hTab, hPage[NPAGES];
static int  curPage, uiLoading;

static const WCHAR* const PAGE_NAMES[NPAGES] = { L"General", L"Hotkeys", L"Languages", L"Words", L"AI", L"Apps" };
static const int PAGE_IDS[NPAGES] = { IDD_GENERAL, IDD_HOTKEYS, IDD_LANGS, IDD_WORDS, IDD_AI, IDD_APPS };

/* Hotkey boxes; each one's Clear button has the next id. */
static const struct { int id; DWORD* slot; const WCHAR* reg; const WCHAR* name; } HOTKEYS[] = {
  { IDC_HK_CONVERT, &setConvertHk, L"ConvertHotkey", L"converting a word" },
  { IDC_HK_TOGGLE,  &setToggleHk,  L"ToggleHotkey",  L"turning LangFix on and off" },
  { IDC_HK_SEL,     &setSelHk,     L"SelHotkey",     L"converting selected text" },
  { IDC_AI_MENUHK,  &aiMenuHk,     L"AiMenuHotkey",  L"the AI menu" },
  { IDC_AI_FIXHK,   &aiFixHk,      L"AiFixHotkey",   L"AI fix writing" },
};
#define NHOTKEYS (int)(sizeof HOTKEYS / sizeof HOTKEYS[0])

static HWND item(int id) {
  HWND h;
  int i;
  for (i = 0; i < NPAGES; i++)
    if (hPage[i] && (h = GetDlgItem(hPage[i], id)) != 0) return h;
  return 0;
}

static void check(int id, BOOL on) { SendMessageW(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static BOOL checked(int id) { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

static void status(const WCHAR* s) {
  if (hDlg) SetDlgItemTextW(hDlg, IDC_STATUS, s);
}

void ui_stats(void) {
  WCHAR s[200];
  if (!hDlg) return;
  wsprintfW(s, L"Since LangFix started: %d word%s fixed, %d converted by hand, %d undone, %d AI edit%s.",
            fixCount, fixCount == 1 ? L"" : L"s", convCount, undoCount, aiCount, aiCount == 1 ? L"" : L"s");
  SetWindowTextW(item(IDC_STATS), s);
  check(IDC_ENABLED, enabled);
  check(IDC_AUTOSTART, autostart_get());
}

void ui_ai_result(const WCHAR* text) {
  if (hDlg) SetWindowTextW(item(IDC_AI_RESULT), text);
}

/* ---------- Languages ---------- */

/* "English" / "Hebrew (Israel)" and the keyboard name ("US", "Hebrew (Standard)") of a group. */
static void group_names(int g, WCHAR* lang, int nl, WCHAR* kbd, int nk) {
  WCHAR name[128], klid[KL_NAMELENGTH], key[96], sub[16], id[8];
  const LAYOUT* L = &g_lay[g];
  DWORD dev = (DWORD)(UINT_PTR)L->hkl[0] >> 16, sz, i, n, v;
  HKEY hk;
  int j;
  lang[0] = kbd[0] = 0;
  for (j = 0; j < L->nloc; j++) {          /* "English", not "English (UK), English (US)" */
    if (!GetLocaleInfoEx(L->loc[j], j || L->nloc > 1 ? LOCALE_SLOCALIZEDLANGUAGENAME : LOCALE_SLOCALIZEDDISPLAYNAME, name, 128))
      wcopy(name, L->loc[j], 128);
    if (j && weq(name, lang)) continue;
    if (lang[0]) wcat(lang, L", ", nl);
    wcat(lang, name, nl);
  }
  /* layout id: 0xFxxx means "Layout Id" xxx of a variant, otherwise the KLID itself */
  wsprintfW(klid, L"%08lx", dev);
  if ((dev & 0xF000) == 0xF000 &&
      !RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts", 0, KEY_READ, &hk)) {
    for (i = 0; sz = 16, !RegEnumKeyExW(hk, i, sub, &sz, 0, 0, 0, 0); i++) {
      n = sizeof id;
      if (RegGetValueW(hk, sub, L"Layout Id", RRF_RT_REG_SZ, 0, id, &n)) continue;
      for (v = 0, j = 0; id[j]; j++) v = v * 16 + (id[j] <= L'9' ? id[j] - L'0' : (id[j] | 32) - L'a' + 10);
      if (v == (dev & 0x0FFF)) { wcopy(klid, sub, KL_NAMELENGTH); break; }
    }
    RegCloseKey(hk);
  }
  wcopy(key, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\", 96);
  wcat(key, klid, 96);
  sz = (DWORD)(nk * sizeof(WCHAR));
  if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Layout Text", RRF_RT_REG_SZ, 0, kbd, &sz)) kbd[0] = 0;
}

static void fill_langs(void) {
  HWND lv = item(IDC_LANGLIST);
  LVITEMW it;
  DICTQ q[MAXL];
  WCHAR lang[256], kbd[96];
  const WCHAR* how;
  int g, spell, asked;
  if (!lv) return;
  /* one short batch: this thread also runs the keyboard hook and must not stall */
  for (g = 0; g < g_nlay; g++) {
    q[g].g = g; q[g].w = L"a"; q[g].n = 1; q[g].typo = 0; q[g].res = -1;
  }
  asked = spell_ask(q, g_nlay, 100);
  uiLoading = 1;
  ListView_DeleteAllItems(lv);
  for (g = 0; g < g_nlay; g++) {
    group_names(g, lang, 256, kbd, 96);
    spell = asked && g_lay[g].nloc && q[g].res >= 0;
    how = spell ? (g_lay[g].model >= 0 ? L"Spellchecker + 30k words" : L"Spellchecker")
                : (g_lay[g].model >= 0 ? L"30k words" : L"Letter patterns");
    zero(&it, sizeof it);
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = g;
    it.pszText = lang;
    it.lParam = g;
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, g, 1, kbd);
    ListView_SetItemText(lv, g, 2, (LPWSTR)how);
    ListView_SetCheckState(lv, g, g_lay[g].enabled);
  }
  uiLoading = 0;
}

void ui_langs_changed(void) { if (hDlg) fill_langs(); }

/* ---------- Words & Apps lists ---------- */

static void fill_list(int id, WCHAR (*list)[MAXW + 1], int n) {
  HWND lb = item(id);
  int i;
  SendMessageW(lb, LB_RESETCONTENT, 0, 0);
  for (i = 0; i < n; i++) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)list[i]);
}

static BOOL CALLBACK add_running_app(HWND w, LPARAM lp) {
  WCHAR exe[MAXW + 1], title[8];
  HWND combo = (HWND)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) ||
      !GetWindowTextW(w, title, 8) || pid == GetCurrentProcessId() || !exe_of_window(w, exe, MAXW + 1))
    return TRUE;
  if (SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)exe) == CB_ERR)
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)exe);
  return TRUE;
}

static void fill_apps(void) {
  HWND combo = item(IDC_APPCOMBO);
  fill_list(IDC_APPLIST, apps, napps);
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  EnumWindows(add_running_app, (LPARAM)combo);
  if (lastAppExe[0]) SetWindowTextW(combo, lastAppExe);
}

/* Add / remove a word or app: which = IDC_MYADD, IDC_MYDEL, IDC_IGNADD, IDC_IGNDEL, IDC_APPADD, IDC_APPDEL. */
static void edit_list(int which) {
  WCHAR raw[128], w[MAXW + 1];
  WCHAR (*arr)[MAXW + 1];
  int *n, max, listId, editId, add, at;
  const WCHAR* reg;
  if (which == IDC_MYADD || which == IDC_MYDEL) {
    arr = g_user; n = &g_nuser; max = MAXUSER; listId = IDC_MYLIST; editId = IDC_MYEDIT; reg = L"MyWords";
  } else if (which == IDC_IGNADD || which == IDC_IGNDEL) {
    arr = ign; n = &nign; max = MAXIGN; listId = IDC_IGNLIST; editId = IDC_IGNEDIT; reg = L"Ignore";
  } else {
    arr = apps; n = &napps; max = MAXAPPS; listId = IDC_APPLIST; editId = IDC_APPCOMBO; reg = L"Apps";
  }
  add = which == IDC_MYADD || which == IDC_IGNADD || which == IDC_APPADD;
  if (add) {
    GetWindowTextW(item(editId), raw, 128);
    if (!clean_word(raw, w)) { status(listId == IDC_APPLIST ? L"Pick an app or type its .exe name." : L"Type one word (no spaces), then Add."); return; }
    if (listId == IDC_APPLIST && (wlen(w) < 5 || !weq(w + wlen(w) - 4, L".exe"))) wcat(w, L".exe", MAXW + 1);
    if (list_find(arr, *n, w) >= 0) { status(L"That is already in the list."); return; }
    if (*n == max) { status(L"The list is full."); return; }
    wcopy(arr[(*n)++], w, MAXW + 1);
    SetWindowTextW(item(editId), L"");
  } else {
    at = (int)SendMessageW(item(listId), LB_GETCURSEL, 0, 0);
    if (at < 0) { status(L"Select an entry to remove."); return; }
    SendMessageW(item(listId), LB_GETTEXT, (WPARAM)at, (LPARAM)raw);
    if ((at = list_find(arr, *n, raw)) >= 0) list_remove(arr, n, at);
  }
  list_save(reg, arr, *n);
  reset_all();
  fill_list(listId, arr, *n);
  status(L"Saved.");
}

/* ---------- hotkeys ---------- */

int hk_valid(DWORD hk) {
  BYTE vk = LOBYTE(hk), m = HIBYTE(hk) & 7;
  if (!vk) return 0;
  if (m & (HOTKEYF_CONTROL | HOTKEYF_ALT)) return 1;
  return (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL;
}

static void hotkey_changed(int k) {
  WCHAR s[160];
  DWORD hk = (DWORD)SendMessageW(item(HOTKEYS[k].id), HKM_GETHOTKEY, 0, 0) & 0xFFFF;
  int j;
  if (!LOBYTE(hk)) return;                       /* still holding modifiers only */
  if (!hk_valid(hk)) {
    status(L"Use Ctrl or Alt with that key, or pick F1\x2013" L"F24 / Pause / Scroll Lock.");
    SendMessageW(item(HOTKEYS[k].id), HKM_SETHOTKEY, *HOTKEYS[k].slot, 0);
    return;
  }
  for (j = 0; j < NHOTKEYS; j++)
    if (j != k && (*HOTKEYS[j].slot & 0x7FF) == (hk & 0x7FF)) {
      wsprintfW(s, L"That shortcut is already used for %s.", HOTKEYS[j].name);
      status(s);
      SendMessageW(item(HOTKEYS[k].id), HKM_SETHOTKEY, *HOTKEYS[k].slot, 0);
      return;
    }
  *HOTKEYS[k].slot = hk;
  reg_set_dword(HOTKEYS[k].reg, hk);
  status(L"Hotkey saved.");
}

/* ---------- loading the pages ---------- */

static void load_pages(void) {
  HWND lv = item(IDC_LANGLIST), model = item(IDC_AI_MODEL);
  LVCOLUMNW col;
  RECT rc;
  WCHAR key[256];
  int w, i;
  uiLoading = 1;
  check(IDC_ENABLED, enabled);
  check(IDC_AUTOSTART, autostart_get());
  check(IDC_STARTMENU, shortcut_get());
  check(IDC_HINTS, setHints);
  check(IDC_ENTER, setEnter);
  check(IDC_UPDATES, setUpdates);
  CheckRadioButton(hPage[PAGE_GENERAL], IDC_SENS0, IDC_SENS2, IDC_SENS0 + (int)(setSens < 3 ? setSens : 1));

  check(IDC_HK_PAUSE, setPause);
  check(IDC_HK_SHIFTS, setShifts);
  check(IDC_HK_SELPAUSE, setSelPause);
  for (i = 0; i < NHOTKEYS; i++) SendMessageW(item(HOTKEYS[i].id), HKM_SETHOTKEY, *HOTKEYS[i].slot, 0);

  ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  GetClientRect(lv, &rc);
  w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
  zero(&col, sizeof col);
  col.mask = LVCF_TEXT | LVCF_WIDTH;
  col.pszText = L"Language";      col.cx = w * 30 / 100; ListView_InsertColumn(lv, 0, &col);
  col.pszText = L"Keyboard";      col.cx = w * 30 / 100; ListView_InsertColumn(lv, 1, &col);
  col.pszText = L"Recognition";   col.cx = w - w * 60 / 100; ListView_InsertColumn(lv, 2, &col);
  fill_langs();

  fill_list(IDC_MYLIST, g_user, g_nuser);
  fill_list(IDC_IGNLIST, ign, nign);
  check(IDC_SLANG, g_par.slang);

  check(IDC_AI_ON, aiEnabled);
  for (i = 0; i < NMODELS; i++) SendMessageW(model, CB_ADDSTRING, 0, (LPARAM)ai_model_label(i));
  SendMessageW(model, CB_SETCURSEL, aiModel, 0);
  SendMessageW(item(IDC_AI_KEY), EM_SETLIMITTEXT, 250, 0);
  if (ai_key_get(key, 256)) SetWindowTextW(item(IDC_AI_KEY), key);
  SecureZeroMemory(key, sizeof key);

  fill_apps();
  ui_stats();
  uiLoading = 0;
}

static void on_command(int id, int code) {
  WCHAR key[256];
  int i;
  if (uiLoading) return;
  for (i = 0; i < NHOTKEYS; i++) {
    if (id == HOTKEYS[i].id && code == EN_CHANGE) { hotkey_changed(i); return; }
    if (id == HOTKEYS[i].id + 1) {               /* Clear */
      SendMessageW(item(HOTKEYS[i].id), HKM_SETHOTKEY, 0, 0);
      *HOTKEYS[i].slot = 0;
      reg_set_dword(HOTKEYS[i].reg, 0);
      status(L"Hotkey removed.");
      return;
    }
  }
  switch (id) {
  case IDC_ENABLED: set_enabled(checked(id)); break;
  case IDC_AUTOSTART: autostart_set(checked(id)); break;
  case IDC_STARTMENU: shortcut_set(checked(id)); break;
  case IDC_HINTS: reg_set_dword(L"Hints", setHints = checked(id)); break;
  case IDC_ENTER: reg_set_dword(L"EnterKey", setEnter = checked(id)); break;
  case IDC_UPDATES:
    reg_set_dword(L"Updates", setUpdates = checked(id));
    if (setUpdates) update_check(TRUE);
    break;
  case IDC_SENS0: case IDC_SENS1: case IDC_SENS2:
    setSens = (DWORD)(id - IDC_SENS0);
    reg_set_dword(L"Sensitivity", setSens);
    apply_engine_settings();
    break;
  case IDC_HK_PAUSE: reg_set_dword(L"PauseKey", setPause = checked(id)); break;
  case IDC_HK_SHIFTS: reg_set_dword(L"ShiftKeys", setShifts = checked(id)); break;
  case IDC_HK_SELPAUSE: reg_set_dword(L"SelPause", setSelPause = checked(id)); break;
  case IDC_LANGSETTINGS: ShellExecuteW(hDlg, L"open", L"ms-settings:regionlanguage", 0, 0, SW_SHOWNORMAL); break;
  case IDC_MYADD: case IDC_MYDEL: case IDC_IGNADD: case IDC_IGNDEL: case IDC_APPADD: case IDC_APPDEL:
    edit_list(id);
    break;
  case IDC_SLANG:
    g_par.slang = checked(id);
    reg_set_dword(L"Slang", (DWORD)g_par.slang);
    break;
  case IDC_AI_ON: reg_set_dword(L"AiEnabled", aiEnabled = checked(id)); break;
  case IDC_AI_MODEL:
    if (code == CBN_SELCHANGE) {
      aiModel = (DWORD)SendMessageW(item(id), CB_GETCURSEL, 0, 0);
      if (aiModel >= NMODELS) aiModel = 0;
      reg_set_dword(L"AiModel", aiModel);
      status(L"Model saved.");
    }
    break;
  case IDC_AI_SAVEKEY:
    GetWindowTextW(item(IDC_AI_KEY), key, 256);
    ai_key_set(key);
    status(key[0] ? L"Key saved, encrypted for your Windows account." : L"Key removed.");
    SecureZeroMemory(key, sizeof key);
    break;
  case IDC_AI_TEST:
    GetWindowTextW(item(IDC_AI_KEY), key, 256);
    if (key[0]) ai_key_set(key);
    SecureZeroMemory(key, sizeof key);
    ui_ai_result(L"Asking Claude\x2026");
    ai_test();
    break;
  }
}

static void on_notify(const NMHDR* nm) {
  const NMLISTVIEW* lv = (const NMLISTVIEW*)nm;
  int g;
  if (nm->code == NM_CLICK || nm->code == NM_RETURN) {
    if (nm->idFrom == IDC_ABOUT || nm->idFrom == IDC_AI_KEYLINK)
      ShellExecuteW(hDlg, L"open", ((const NMLINK*)nm)->item.szUrl, 0, 0, SW_SHOWNORMAL);
    return;
  }
  if (uiLoading || nm->idFrom != IDC_LANGLIST || nm->code != LVN_ITEMCHANGED) return;
  if (!(lv->uChanged & LVIF_STATE) || !((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) return;
  g = (int)lv->lParam;
  if (g < 0 || g >= g_nlay) return;
  g_lay[g].enabled = ListView_GetCheckState(nm->hwndFrom, lv->iItem) != 0;
  layouts_save();
  reset_all();
  status(g_lay[g].enabled ? L"Layout switched on." : L"Layout switched off.");
}

static INT_PTR CALLBACK PageProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  (void)h;
  switch (m) {
  case WM_INITDIALOG: return TRUE;
  case WM_COMMAND: on_command(LOWORD(w), HIWORD(w)); return TRUE;
  case WM_NOTIFY: on_notify((const NMHDR*)l); return TRUE;
  }
  return FALSE;
}

static void select_page(int i) {
  if (i < 0 || i >= NPAGES || !hPage[i]) return;
  TabCtrl_SetCurSel(hTab, i);
  ShowWindow(hPage[curPage], SW_HIDE);
  ShowWindow(hPage[i], SW_SHOW);
  curPage = i;
}

static INT_PTR CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  TCITEMW ti;
  RECT rc;
  HWND f;
  int i;
  switch (m) {
  case WM_INITDIALOG:
    hDlg = h;
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)icoBig);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)icoOn);
    hTab = GetDlgItem(h, IDC_TAB);
    for (i = 0; i < NPAGES; i++) {
      ti.mask = TCIF_TEXT;
      ti.pszText = (LPWSTR)PAGE_NAMES[i];
      TabCtrl_InsertItem(hTab, i, &ti);
    }
    GetWindowRect(hTab, &rc);
    MapWindowPoints(0, h, (POINT*)&rc, 2);
    TabCtrl_AdjustRect(hTab, FALSE, &rc);
    for (i = 0; i < NPAGES; i++) {
      hPage[i] = CreateDialogParamW(hInst, MAKEINTRESOURCEW(PAGE_IDS[i]), h, PageProc, 0);
      EnableThemeDialogTexture(hPage[i], ETDT_ENABLETAB);
      SetWindowPos(hPage[i], HWND_TOP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOACTIVATE | (i ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
    }
    curPage = 0;
    load_pages();
    return TRUE;
  case WM_NOTIFY:
    if (((NMHDR*)l)->idFrom == IDC_TAB && ((NMHDR*)l)->code == TCN_SELCHANGE) select_page(TabCtrl_GetCurSel(hTab));
    return TRUE;
  case WM_COMMAND:
    if (LOWORD(w) == IDOK) {           /* Enter: add what is typed in a word / app box */
      f = GetFocus();
      if (f == item(IDC_MYEDIT)) edit_list(IDC_MYADD);
      else if (f == item(IDC_IGNEDIT)) edit_list(IDC_IGNADD);
      else if (f == item(IDC_APPCOMBO) || GetParent(f) == item(IDC_APPCOMBO)) edit_list(IDC_APPADD);
      else if (f == item(IDC_AI_KEY)) on_command(IDC_AI_SAVEKEY, BN_CLICKED);
      return TRUE;
    }
    if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
    return TRUE;
  case WM_CLOSE:
    DestroyWindow(h);
    return TRUE;
  case WM_DESTROY:
    hDlg = hTab = 0;
    zero(hPage, sizeof hPage);
    return TRUE;
  }
  return FALSE;
}

/* page < 0 keeps the current tab. */
void ui_show(int page) {
  if (!hDlg) CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_MAIN), 0, MainProc, 0);
  if (!hDlg) return;
  if (page >= 0) select_page(page);
  ShowWindow(hDlg, SW_SHOWNORMAL);
  SetForegroundWindow(hDlg);
}

/* ---------- "Ask Claude" ---------- */

static WCHAR* askBuf;
static int    askMax;

static INT_PTR CALLBACK AskProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  WCHAR last[700];
  (void)l;
  switch (m) {
  case WM_INITDIALOG:
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)icoOn);
    SendDlgItemMessageW(h, IDC_ASK_EDIT, EM_SETLIMITTEXT, (WPARAM)(askMax - 1), 0);
    if (reg_str(L"AiLastAsk", last, 700)) {
      SetDlgItemTextW(h, IDC_ASK_EDIT, last);
      SendDlgItemMessageW(h, IDC_ASK_EDIT, EM_SETSEL, 0, -1);
    }
    SetForegroundWindow(h);
    SetFocus(GetDlgItem(h, IDC_ASK_EDIT));
    return FALSE;
  case WM_COMMAND:
    if (LOWORD(w) == IDOK) {
      GetDlgItemTextW(h, IDC_ASK_EDIT, askBuf, askMax);
      if (!askBuf[0]) return TRUE;
      reg_set_str(L"AiLastAsk", askBuf);
      EndDialog(h, 1);
    } else if (LOWORD(w) == IDCANCEL) {
      EndDialog(h, 0);
    }
    return TRUE;
  }
  return FALSE;
}

int ui_ask(HWND owner, WCHAR* instr, int max) {
  askBuf = instr;
  askMax = max;
  instr[0] = 0;
  return DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_ASK), owner, AskProc, 0) == 1 && instr[0];
}
