/* End-to-end test: starts LangFix.exe, opens its own window with an edit box, presses
   physical keys with SendInput and checks the text and keyboard layout that result.
   e2e <LangFix.exe> <report.txt>
   Keys only go out while the test window is in the foreground; otherwise the test stops. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

static HWND wnd, edit;
static FILE* out;
static int fails, total;

static void pump(int ms) {
  DWORD t0 = GetTickCount();
  MSG m;
  do {
    while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
    Sleep(1);
  } while ((int)(GetTickCount() - t0) < ms);
}

static int ensure_front(void) {
  if (GetForegroundWindow() == wnd && !edit) return 1;
  if (GetForegroundWindow() == wnd) {
    SetFocus(edit);
    pump(20);
    return GetFocus() == edit;
  }
  {
    HWND fg = GetForegroundWindow();
    DWORD ft = GetWindowThreadProcessId(fg, 0), me = GetCurrentThreadId();
    AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(wnd);
    SetFocus(edit);
    AttachThreadInput(me, ft, FALSE);
  }
  pump(100);
  if (!edit) return GetForegroundWindow() == wnd;
  SetFocus(edit);
  return GetForegroundWindow() == wnd && GetFocus() == edit;
}

static void key(WORD vk, int up) {
  INPUT i = {0};
  i.type = INPUT_KEYBOARD;
  i.ki.wVk = vk;
  i.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
  SendInput(1, &i, sizeof i);
}

/* Key spec: a-z letters, A-Z shift+letter, space, , . ; ' [ ] ` / digits,
   '<' Backspace, '|' Pause, '>' End, '~' Enter, '^' both Shift keys, '!' Shift+Pause,
   Ctrl+Alt+ '#' Q, '@' P, '&' Enter, '%' Space. delay < 0 sends everything in one burst. */
static void press(const char* s, int delay) {
  static INPUT burst[512];
  int nb = 0;
  for (; *s; s++) {
    WORD vk = 0;
    int shift = 0;
    char c = *s;
    if (c >= 'a' && c <= 'z') vk = (WORD)('A' + c - 'a');
    else if (c >= 'A' && c <= 'Z') { vk = (WORD)c; shift = 1; }
    else if (c >= '0' && c <= '9') vk = (WORD)c;
    else switch (c) {
      case ' ': vk = VK_SPACE; break;
      case ',': vk = VK_OEM_COMMA; break;
      case '.': vk = VK_OEM_PERIOD; break;
      case ';': vk = VK_OEM_1; break;
      case '\'': vk = VK_OEM_7; break;
      case '[': vk = VK_OEM_4; break;
      case ']': vk = VK_OEM_6; break;
      case '`': vk = VK_OEM_3; break;
      case '/': vk = VK_OEM_2; break;
      case '<': vk = VK_BACK; break;
      case '|': vk = VK_PAUSE; break;
      case '>': vk = VK_END; break;
      case '^':
        key(VK_LSHIFT, 0); pump(20); key(VK_RSHIFT, 0); pump(20); key(VK_RSHIFT, 1); key(VK_LSHIFT, 1);
        pump(delay > 0 ? delay : 30);
        continue;
      case '#': case '@': case '&': case '%':  /* Ctrl+Alt + Q / P / Enter / Space, Alt released last */
        vk = c == '#' ? 'Q' : c == '@' ? 'P' : c == '&' ? VK_RETURN : VK_SPACE;
        key(VK_LCONTROL, 0); pump(15); key(VK_LMENU, 0); pump(15);
        key(vk, 0); key(vk, 1); pump(15);
        key(VK_LCONTROL, 1); pump(15); key(VK_LMENU, 1);
        pump(delay > 0 ? delay + 60 : 80);
        continue;
      case '!':                            /* Shift+Pause */
        key(VK_LSHIFT, 0); key(VK_PAUSE, 0); key(VK_PAUSE, 1); key(VK_LSHIFT, 1);
        pump(delay > 0 ? delay : 30);
        continue;
      case '~': vk = VK_RETURN; break;
    }
    if (!vk) continue;
    if (delay < 0) {
      INPUT i = {0};
      i.type = INPUT_KEYBOARD;
      if (shift) { i.ki.wVk = VK_LSHIFT; burst[nb++] = i; }
      i.ki.wVk = vk; i.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); burst[nb++] = i;
      i.ki.dwFlags = KEYEVENTF_KEYUP; burst[nb++] = i;
      if (shift) { i.ki.wVk = VK_LSHIFT; i.ki.wScan = 0; burst[nb++] = i; }
      continue;
    }
    if (shift) key(VK_LSHIFT, 0);
    key(vk, 0);
    key(vk, 1);
    if (shift) key(VK_LSHIFT, 1);
    pump(delay);
  }
  if (nb) SendInput((UINT)nb, burst, sizeof(INPUT));
}

static HKL find_hkl(WORD lang) {
  HKL l[32];
  int n = GetKeyboardLayoutList(32, l), i;
  for (i = 0; i < n; i++) if (LOWORD((UINT_PTR)l[i]) == lang) return l[i];
  return 0;
}

/* --edge mode: the text box is a <textarea> in an Edge app window whose title mirrors it
   (line breaks shown as '|', since a title can't hold them). */
static int external;

static void ext_text(WCHAR* got, int max) {
  WCHAR t[512], *a, *b;
  int n = 0;
  got[0] = 0;
  GetWindowTextW(wnd, t, 512);
  a = wcsstr(t, L"LFTEST[");
  b = wcsrchr(t, L']');
  if (!a || !b || b < a + 7) return;
  *b = 0;
  for (a += 7; *a && n < max - 2; a++) {
    if (*a == L'|') { got[n++] = L'\r'; got[n++] = L'\n'; }
    else got[n++] = *a;
  }
  got[n] = 0;
}

static void ctrl_a_delete(void) {
  INPUT i[6] = {0};
  int n;
  for (n = 0; n < 6; n++) i[n].type = INPUT_KEYBOARD;
  i[0].ki.wVk = VK_LCONTROL; i[1].ki.wVk = 'A'; i[2].ki.wVk = 'A'; i[2].ki.dwFlags = KEYEVENTF_KEYUP;
  i[3].ki.wVk = VK_LCONTROL; i[3].ki.dwFlags = KEYEVENTF_KEYUP;
  i[4].ki.wVk = VK_BACK; i[5].ki.wVk = VK_BACK; i[5].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(6, i, sizeof(INPUT));
}

static BOOL CALLBACK find_edge(HWND h, LPARAM lp) {
  WCHAR t[64];
  (void)lp;
  if (IsWindowVisible(h) && GetWindowTextW(h, t, 64) && wcsstr(t, L"LFTEST[")) { wnd = h; return FALSE; }
  return TRUE;
}

static void check(const char* name, HKL start, const char* keys, int delay, const WCHAR* want, HKL wantLayout) {
  WCHAR got[256];
  HKL now;
  int ok;
  total++;
  if (!external) {
    SetWindowTextW(edit, L"");
    ActivateKeyboardLayout(start, 0);
    pump(60);
  }
  if (!ensure_front()) { fwprintf(out, L"ABORT: test window lost focus before %S\n", name); fails++; return; }
  if (external) {
    ctrl_a_delete();
    pump(150);
    PostMessageW(wnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)start);
    pump(150);
    if (GetForegroundWindow() != wnd) { fwprintf(out, L"ABORT: focus lost in %S\n", name); fails++; return; }
  }
  press(">", 20);                      /* End: a non-text key, so LangFix starts a fresh word */
  press(keys, delay);
  pump(external ? 700 : 400);
  if (external) {
    ext_text(got, 256);
    now = GetKeyboardLayout(GetWindowThreadProcessId(wnd, 0));
  } else {
    GetWindowTextW(edit, got, 256);
    now = GetKeyboardLayout(0);
  }
  ok = !wcscmp(got, want) &&
       (!wantLayout || PRIMARYLANGID(LOWORD((UINT_PTR)now)) == PRIMARYLANGID(LOWORD((UINT_PTR)wantLayout)));
  if (!ok) fails++;
  fwprintf(out, L"%s  %-34S got \"%s\"  want \"%s\"  layout %04x%s\n", ok ? L"PASS" : L"FAIL", name, got, want,
           LOWORD((UINT_PTR)now), wantLayout ? L"" : L" (any)");
}

/* Like check(), but the box starts with `init` all selected (0 = empty box) and the
   result is read after `wait` ms - for selected-text actions and the AI. */
static void check_sel(const char* name, HKL start, const WCHAR* init, const char* keys, int delay, int wait,
                      const WCHAR* want, HKL wantLayout) {
  WCHAR got[256];
  HKL now;
  int ok;
  total++;
  SetWindowTextW(edit, init ? init : L"");
  ActivateKeyboardLayout(start, 0);
  pump(60);
  if (!ensure_front()) { fwprintf(out, L"ABORT: test window lost focus before %S\n", name); fails++; return; }
  if (init) SendMessageW(edit, EM_SETSEL, 0, -1);
  else press(">", 20);
  press(keys, delay);
  pump(wait);
  GetWindowTextW(edit, got, 256);
  now = GetKeyboardLayout(0);
  ok = !wcscmp(got, want) &&
       (!wantLayout || PRIMARYLANGID(LOWORD((UINT_PTR)now)) == PRIMARYLANGID(LOWORD((UINT_PTR)wantLayout)));
  if (!ok) fails++;
  fwprintf(out, L"%s  %-34S got \"%s\"  want \"%s\"  layout %04x%s\n", ok ? L"PASS" : L"FAIL", name, got, want,
           LOWORD((UINT_PTR)now), wantLayout ? L"" : L" (any)");
}

static void clip_put(const WCHAR* s) {
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wcslen(s) + 1) * sizeof(WCHAR));
  wcscpy_s((WCHAR*)GlobalLock(h), wcslen(s) + 1, s);
  GlobalUnlock(h);
  OpenClipboard(wnd);
  EmptyClipboard();
  SetClipboardData(CF_UNICODETEXT, h);
  CloseClipboard();
}

static void clip_check(const char* name, const WCHAR* want) {
  WCHAR got[256] = L"";
  HANDLE h;
  int ok;
  total++;
  if (OpenClipboard(wnd)) {
    if ((h = GetClipboardData(CF_UNICODETEXT)) != 0) { wcsncpy_s(got, 256, (WCHAR*)GlobalLock(h), _TRUNCATE); GlobalUnlock(h); }
    CloseClipboard();
  }
  ok = !wcscmp(got, want);
  if (!ok) fails++;
  fwprintf(out, L"%s  %-34S got \"%s\"  want \"%s\"\n", ok ? L"PASS" : L"FAIL", name, got, want);
}

static PROCESS_INFORMATION lfp;

static int lf_start(const WCHAR* exe) {
  STARTUPINFOW si = { sizeof si };
  static WCHAR cl[MAX_PATH + 32];
  swprintf_s(cl, MAX_PATH + 32, L"\"%s\" --tray", exe);
  if (!CreateProcessW(exe, cl, 0, 0, FALSE, 0, 0, 0, &si, &lfp)) return 0;
  Sleep(1500);                         /* hooks in, dictionaries warm */
  return 1;
}

static void lf_stop(void) {
  HWND lf = FindWindowW(L"LangFixWnd", 0);
  if (lf) PostMessageW(lf, WM_CLOSE, 0, 0);
  if (WaitForSingleObject(lfp.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(lfp.hProcess, 1);
  CloseHandle(lfp.hProcess);
  CloseHandle(lfp.hThread);
}

static void reg_msz(const WCHAR* name, const WCHAR* one) {
  WCHAR buf[64] = {0};
  wcscpy_s(buf, 62, one);
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\LangFix", name, REG_MULTI_SZ, buf, (DWORD)((wcslen(one) + 2) * sizeof(WCHAR)));
}

static void reg_sz(const WCHAR* name, const WCHAR* s) {
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\LangFix", name, REG_SZ, s, (DWORD)((wcslen(s) + 1) * sizeof(WCHAR)));
}

static void reg_dw(const WCHAR* name, DWORD v) {
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\LangFix", name, REG_DWORD, &v, sizeof v);
}

int wmain(int argc, wchar_t** argv) {
  HKL en, he, ru, ruLoaded = 0;
  WNDCLASSW wc = {0};
  HKEY hk;
  WCHAR hex[16];
  int hadSettings;

  if (argc < 3 || _wfopen_s(&out, argv[2], L"w, ccs=UTF-8")) return 2;
  hadSettings = !RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LangFix", 0, KEY_READ, &hk);
  if (hadSettings) {
    RegCloseKey(hk);
    fwprintf(out, L"LangFix settings already exist in HKCU\\Software\\LangFix; not touching them.\n");
    fclose(out);
    return 2;
  }
  en = find_hkl(0x0409);
  he = find_hkl(0x040D);
  ru = find_hkl(0x0419);
  if (!ru) ru = ruLoaded = LoadKeyboardLayoutW(L"00000419", KLF_NOTELLSHELL);
  fwprintf(out, L"layouts: en=%p he=%p ru=%p%s\n", en, he, ru, ruLoaded ? L" (loaded for the test)" : L"");

  if (!lf_start(argv[1])) { fwprintf(out, L"cannot start LangFix\n"); return 2; }

  if (argc >= 5 && !wcscmp(argv[3], L"--edge")) {
    /* argv[4] = msedge.exe, argv[5] = throwaway profile folder */
    static WCHAR cmd[2048];
    STARTUPINFOW si2 = { sizeof si2 };
    PROCESS_INFORMATION pi2;
    int i;
    external = 1;
    swprintf_s(cmd, 2048, L"\"%s\" --user-data-dir=\"%s\" --no-first-run --no-default-browser-check --disable-sync "
               L"--window-size=700,300 \"--app=data:text/html,<title>LFTEST[]</title><textarea autofocus "
               L"style=width:95vw;height:80vh oninput=document.title='LFTEST['+this.value.split(String.fromCharCode(10)).join('|')+']'></textarea>\"",
               argv[4], argc >= 6 ? argv[5] : L"lftest-profile");
    if (!CreateProcessW(0, cmd, 0, 0, FALSE, 0, 0, 0, &si2, &pi2)) { fwprintf(out, L"cannot start Edge\n"); return 2; }
    for (i = 0; i < 150 && !wnd; i++) { Sleep(100); EnumWindows(find_edge, 0); }
    if (!wnd) { fwprintf(out, L"Edge window not found\n"); return 2; }
    Sleep(1500);
    fwprintf(out, L"target: Edge app window %p\n", wnd);
  } else {
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"LangFixE2E";
    RegisterClassW(&wc);
    wnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"LangFix test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          200, 200, 700, 200, 0, 0, wc.hInstance, 0);
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
                           0, 0, 680, 160, wnd, 0, wc.hInstance, 0);
    pump(200);
  }

  check("he layout: hello", he, "hello ", 15, L"hello ", en);
  check("he layout: Hello (shift)", he, "Hello ", 15, L"Hello ", en);
  check("en layout: correct hello stays", en, "hello world ", 15, L"hello world ", en);
  check("en layout: ghbdtn -> privet", en, "ghbdtn ", 15, L"\x043F\x0440\x0438\x0432\x0435\x0442 ", ru);
  check("en layout: Ghbdtn capital", en, "Ghbdtn ", 15, L"\x041F\x0440\x0438\x0432\x0435\x0442 ", ru);
  check("en layout: akuo -> shalom", en, "akuo ", 15, L"\x05E9\x05DC\x05D5\x05DD ", he);
  check("ru layout: nuy -> yes", ru, "yes ", 15, L"yes ", en);
  check("backspace inside word", en, "ghbdtm<n ", 15, L"\x043F\x0440\x0438\x0432\x0435\x0442 ", ru);
  check("digits are never touched", en, "ghbdtn1 ", 15, L"ghbdtn1 ", en);
  check("fast burst: 3 words at once", en, "ghbdtn rfr ltkf ", -1,
        L"\x043F\x0440\x0438\x0432\x0435\x0442 \x043A\x0430\x043A \x0434\x0435\x043B\x0430 ", ru);
  check("fast 5ms typing, mixed", he, "hello world ", 5, L"hello world ", en);
  check("ambiguous 'to' fixed with next word", he, "to home ", 15, L"to home ", en);
  check("Pause undoes an automatic fix", he, "hello |", 15, L"\x05D9\x05E7\x05DA\x05DA\x05DD ", he);
  check("undone word is remembered", he, "hello ", 15, L"\x05D9\x05E7\x05DA\x05DA\x05DD ", he);
  check("Pause converts word being typed", en, "rfr|", 15, L"\x043A\x0430\x043A", ru);
  check("Shift+Shift converts word", en, "nen^", 15, L"\x0442\x0443\x0442", ru);
  check("trailing comma stays English", en, "hello, ", 15, L"hello, ", en);
  check("trailing comma fixed to English", he, "hello, ", 15, L"hello, ", en);

  lf_stop();

  /* phase 2: settings from the settings window, written the way LangFix stores them */
  fwprintf(out, L"\n--- with settings: never-change \"akuo\", my word \"\x05D7\x05DE\x05E9\x05D5\x05E9\", Russian off, "
                L"convert = Ctrl+Alt+Q, on/off = Ctrl+Alt+P\n");
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LangFix");
  reg_msz(L"Ignore", L"akuo");
  reg_msz(L"MyWords", L"\x05D7\x05DE\x05E9\x05D5\x05E9");
  swprintf_s(hex, 16, L"%08lx", (DWORD)(UINT_PTR)ru);
  reg_msz(L"Disabled", hex);
  reg_dw(L"ConvertHotkey", 'Q' | (HOTKEYF_CONTROL | HOTKEYF_ALT) << 8);
  reg_dw(L"ToggleHotkey", 'P' | (HOTKEYF_CONTROL | HOTKEYF_ALT) << 8);
  reg_dw(L"EnterKey", 1);
  if (!lf_start(argv[1])) { fwprintf(out, L"cannot restart LangFix\n"); return 2; }
  check("Enter fixes the last word", he, "hello~", 15, L"hello\r\n", en);
  check("never-change word stays", en, "akuo ", 15, L"akuo ", en);
  check("my word gets fixed", en, "jnaua ", 15, L"\x05D7\x05DE\x05E9\x05D5\x05E9 ", he);
  check("Russian switched off: stays", en, "ghbdtn ", 15, L"ghbdtn ", en);
  check("Hebrew still on", he, "hello ", 15, L"hello ", en);
  check("Ctrl+Alt+Q converts word", he, "hello#", 15, L"hello", en);
  check("Ctrl+Alt+P pauses LangFix", he, "@hello ", 15, L"\x05D9\x05E7\x05DA\x05DA\x05DD ", he);
  check("Ctrl+Alt+P resumes LangFix", he, "@hello ", 15, L"hello ", en);
  lf_stop();

  /* phase 3: selected text and the AI tools, against tests/mock_claude.py on port 8765 */
  if (argc < 4 || wcscmp(argv[3], L"--edge")) {
    fwprintf(out, L"\n--- selected text and AI tools (mock Claude API); this test app is on the Apps list\n");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LangFix");
    reg_sz(L"AiBaseUrl", L"http://127.0.0.1:8765");
    reg_msz(L"Apps", L"e2e.exe");
    SetEnvironmentVariableW(L"ANTHROPIC_API_KEY", L"test-key");
    if (!lf_start(argv[1])) { fwprintf(out, L"cannot restart LangFix\n"); return 2; }
    check_sel("Shift+Pause converts a selection", en, L"ghbdtn rfr ltkf", "!", 30, 900,
              L"\x043F\x0440\x0438\x0432\x0435\x0442 \x043A\x0430\x043A \x0434\x0435\x043B\x0430", ru);
    check("listed app: no automatic fix", en, "ghbdtn ", 15, L"ghbdtn ", en);
    clip_put(L"ORIGINAL CLIP");
    check_sel("AI fix: selected text", en, L"make this loud", "&", 30, 2500, L"MAKE THIS LOUD", 0);
    check_sel("AI fix: line before the cursor", en, 0, "fix this line&", 15, 2500, L"FIX THIS LINE", 0);
    check_sel("AI menu: Enter picks Fix", en, L"menu pick", "%~", 900, 2500, L"MENU PICK", 0);
    check_sel("AI error leaves the text alone", en, L"ERROR401 please", "&", 30, 2500, L"ERROR401 please", 0);
    clip_check("clipboard restored after AI", L"ORIGINAL CLIP");
    lf_stop();
  }

  fwprintf(out, L"\n%d/%d passed\n", total - fails, total);
  if (external) PostMessageW(wnd, WM_CLOSE, 0, 0);
  else DestroyWindow(wnd);
  if (ruLoaded) UnloadKeyboardLayout(ruLoaded);
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\LangFix");   /* leave a clean first run */
  fclose(out);
  return fails ? 1 : 0;
}
