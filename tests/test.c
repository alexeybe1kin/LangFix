/* Offline test for the detection engine.
   lftest <out.txt> [-nodict] [-ctx] <lang>:<words.txt> ... [-type <lang>:<text.txt>] ...
   Every word is "typed" (as key presses) in its own layout and then viewed through
   every installed layout:  own layout -> must stay,  other layout -> must switch back.
   Lines starting with '!' are words that must never be switched (names, typos, slang). */
#define COBJMACROS
#include <windows.h>
#include <spellcheck.h>
#include <stdio.h>
#include <wchar.h>
#include "../src/engine.h"

static const CLSID xCLSID_SCF = {0x7AB36653,0x1796,0x484B,{0xBD,0xFA,0xE7,0x4F,0x1D,0xB7,0xC1,0xDC}};
static const IID   xIID_SCF   = {0x8E018A9D,0x2415,0x4677,{0xBF,0x08,0x79,0x4E,0xA6,0x1F,0x94,0xBB}};

static ISpellCheckerFactory* fac;
static struct { WCHAR loc[LOCALE_NAME_MAX_LENGTH]; ISpellChecker* sc; } chk[16];
static int nchk, nodict, usectx;
static FILE* out;

static ISpellChecker* checker(const WCHAR* loc) {
  int i; BOOL sup = FALSE; ISpellChecker* sc = 0;
  for (i = 0; i < nchk; i++) if (!_wcsicmp(chk[i].loc, loc)) return chk[i].sc;
  if (SUCCEEDED(ISpellCheckerFactory_IsSupported(fac, loc, &sup)) && sup)
    ISpellCheckerFactory_CreateSpellChecker(fac, loc, &sc);
  wcscpy_s(chk[nchk].loc, LOCALE_NAME_MAX_LENGTH, loc);
  chk[nchk].sc = sc;
  return chk[nchk++].sc;
}

/* -fakeru: a stand-in Russian dictionary made of the corpus and test words,
   because this PC has no Russian spellchecker. */
static int fakeru;
static WCHAR fake[6000][MAXW + 1];
static int nfake;

static void fake_norm(WCHAR* w) { for (; *w; w++) *w = eng_lower(*w); }

static void fake_load(const WCHAR* path) {
  FILE* f; WCHAR line[4096];
  if (_wfopen_s(&f, path, L"r, ccs=UTF-8")) return;
  while (fgetws(line, 4096, f)) {
    WCHAR* p = line;
    while (*p) {
      int n = 0;
      while (*p && !(eng_script_of(*p) == SC_CYR)) p++;
      while (eng_script_of(p[n]) == SC_CYR || (p[n] == L'-' && eng_script_of(p[n + 1]) == SC_CYR)) n++;
      if (n && n <= MAXW && nfake < 6000) {
        int i;
        wmemcpy(fake[nfake], p, n); fake[nfake][n] = 0; fake_norm(fake[nfake]);
        for (i = 0; i < nfake; i++) if (!wcscmp(fake[i], fake[nfake])) break;
        if (i == nfake) nfake++;
      }
      p += n;
    }
  }
  fclose(f);
}

static int fake_check(const WCHAR* w0, int nearMode) {
  WCHAR w[MAXW + 1]; int i;
  wcscpy_s(w, MAXW + 1, w0); fake_norm(w);
  for (i = 0; i < nfake; i++) {
    if (!nearMode && !wcscmp(fake[i], w)) return 1;
    if (nearMode && eng_one_edit(fake[i], (int)wcslen(fake[i]), w, (int)wcslen(w))) return 1;
  }
  return 0;
}

static int near_word(ISpellChecker* sc, const WCHAR* w) {
  IEnumString* es = 0; LPOLESTR s; ULONG got; int found = 0;
  if (FAILED(ISpellChecker_Suggest(sc, w, &es)) || !es) return 0;
  while (!found && IEnumString_Next(es, 1, &s, &got) == S_OK) {
    found = eng_one_edit(s, (int)wcslen(s), w, (int)wcslen(w));
    CoTaskMemFree(s);
  }
  IEnumString_Release(es);
  return found;
}

static long long dictCalls;   /* how often the engine had to ask the spellchecker */

static int dict(DICTQ* q, int nq) {
  int i, j;
  WCHAR w[MAXW + 1];
  dictCalls++;
  for (i = 0; i < nq; i++) {
    LAYOUT* L = &g_lay[q[i].g];
    q[i].res = -1;
    if (nodict) continue;
    wmemcpy(w, q[i].w, q[i].n); w[q[i].n] = 0;
    if (fakeru && L->script == SC_CYR) { q[i].res = fake_check(w, q[i].typo); continue; }
    for (j = 0; j < L->nloc; j++) {
      ISpellChecker* sc = checker(L->loc[j]);
      IEnumSpellingError* e = 0; ISpellingError* err = 0;
      if (!sc) continue;
      if (q[i].typo) { if (q[i].res < 0) q[i].res = 0; if (near_word(sc, w)) q[i].res = 1; continue; }
      if (FAILED(ISpellChecker_Check(sc, w, &e)) || !e) continue;
      if (IEnumSpellingError_Next(e, &err) == S_OK) { if (q[i].res < 0) q[i].res = 0; ISpellingError_Release(err); }
      else q[i].res = 1;
      IEnumSpellingError_Release(e);
    }
  }
  return 1;
}

static int to_keys(int g, const WCHAR* w, KEY* k) {
  int n = 0, vk, m;
  for (; *w; w++) {
    for (m = 0; m < 2; m++) {
      for (vk = 1; vk < 255; vk++)
        if (g_lay[g].map[vk][m] == *w && !g_lay[g].bad[vk]) break;
      if (vk < 255) break;
    }
    if (m == 2 || n == MAXW) return -1;
    k[n].vk = (BYTE)vk; k[n].mods = (BYTE)m; n++;
  }
  return n;
}

static int group_of_model(int model) {
  int g;
  for (g = 0; g < g_nlay; g++) if (g_lay[g].model == model) return g;
  return -1;
}

static void dump(int cur) {
  int g;
  for (g = 0; g < g_nlay; g++) {
    CAND* C = &g_cand[g];
    fwprintf(out, L"      %c%d %-14s script=%d edge=%d ng=%d dict=%d ev=%d valid=%d\n",
             g == cur ? L'*' : L' ', g, C->t, C->scriptOk, C->edgeOk, C->ng, C->dict,
             C->ev > 100000 ? -1 : C->ev, C->valid);
  }
}

/* -type <lang>:<text.txt>: running text word by word, as the app sees it on Space.
   In its own layout (with the previous word as context) nothing may switch; in every
   other layout each word must switch back. Prints the time per word and how often the
   spellchecker was needed. */
static void type_text(const wchar_t* arg) {
  static const wchar_t* names[] = { L"en", L"ru", L"he" };
  static WCHAR text[1 << 17];
  WCHAR line[4096], w[MAXW + 1], o[MAXW + 1], *p;
  KEY k[MAXW];
  FILE* in;
  LARGE_INTEGER f, t0, t1;
  int model, src, cur, ctx, n, t, len, words, bad, used = 0;
  long long calls0;
  for (model = 0; model < 3; model++) if (!wcsncmp(arg, names[model], 2)) break;
  if (model == 3 || (src = group_of_model(model)) < 0 || _wfopen_s(&in, arg + 3, L"r, ccs=UTF-8")) {
    fwprintf(out, L"skip -type %s\n", arg);
    return;
  }
  while (fgetws(line, 4096, in) && used + 4097 < (int)(sizeof text / sizeof text[0])) {
    wcscpy_s(text + used, 4097, line);
    used += (int)wcslen(line);
  }
  fclose(in);
  fwprintf(out, L"\n=== -type %s\n", arg);
  for (cur = 0; cur < g_nlay; cur++) {   /* load the dictionaries before timing, as the app does at start */
    DICTQ q;
    q.g = cur; q.w = L"a"; q.n = 1; q.typo = 0;
    dict(&q, 1);
  }
  QueryPerformanceFrequency(&f);
  for (cur = 0; cur < g_nlay; cur++) {
    calls0 = dictCalls;
    words = bad = 0;
    ctx = -1;
    QueryPerformanceCounter(&t0);
    for (p = text; *p;) {
      while (*p && iswspace(*p)) p++;
      for (len = 0; p[len] && !iswspace(p[len]); len++) {}
      if (!len) break;
      if (len > MAXW) { p += len; ctx = -1; continue; }
      wmemcpy(w, p, len); w[len] = 0;
      p += len;
      if ((n = to_keys(src, w, k)) < 2) { ctx = -1; continue; }
      t = eng_decide(k, n, cur, cur == src ? ctx : -1, dict, o);
      words++;
      if (cur == src) {
        if (t >= 0) { bad++; fwprintf(out, L"  FALSE-SWITCH %s -> %s\n", w, o); }
        ctx = t >= 0 ? t : g_gain < 0 ? cur : -1;
      } else if (t != src) {
        bad++;
        fwprintf(out, L"  MISSED %s typed in %d -> %d\n", w, cur, t);
      }
    }
    QueryPerformanceCounter(&t1);
    fwprintf(out, L"  typed in %d%s: %d words, %d %s, %.1f us per word, spellchecker asked for %.0f%% of words\n",
             cur, cur == src ? L" (own layout)" : L"", words, bad, cur == src ? L"false switches" : L"missed",
             (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / (words ? words : 1),
             100.0 * (double)(dictCalls - calls0) / (words ? words : 1));
  }
}

/* gain records for the threshold sweep: kind 0 = must switch, 1 = must stay */
static struct { int gain, right, kind, typo, curdict; WCHAR w[64]; } rec[20000];
static int nrec;

int wmain(int argc, wchar_t** argv) {
  HKL ru = 0;
  int a, g, tot[MAXL][MAXL] = {0}, bad[MAXL][MAXL] = {0}, negTot = 0, negBad = 0;
  static const wchar_t* names[] = { L"en", L"ru", L"he" };
  LARGE_INTEGER f, t0, t1; long long calls = 0;

  if (argc < 3) { fwprintf(stderr, L"usage\n"); return 2; }
  if (_wfopen_s(&out, argv[1], L"w, ccs=UTF-8")) return 2;
  CoInitializeEx(0, COINIT_MULTITHREADED);
  CoCreateInstance(&xCLSID_SCF, 0, CLSCTX_INPROC_SERVER, &xIID_SCF, (void**)&fac);

  eng_init_layouts();
  if (group_of_model(1) < 0) {           /* no Russian layout installed: borrow one for the test */
    ru = LoadKeyboardLayoutW(L"00000419", KLF_NOTELLSHELL);
    if (ru) eng_add_hkl(ru);
  }
  for (g = 0; g < g_nlay; g++)
    fwprintf(out, L"group %d: hkl=%p n=%d loc=%s script=%d model=%d\n", g, g_lay[g].hkl[0],
             g_lay[g].nhkl, g_lay[g].nloc ? g_lay[g].loc[0] : L"-", g_lay[g].script, g_lay[g].model);

  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  for (a = 2; a < argc; a++) {
    FILE* in; WCHAR line[256], o[MAXW + 1];
    int model, src;
    if (!wcscmp(argv[a], L"-nodict")) { nodict = 1; continue; }
    if (!wcscmp(argv[a], L"-fakeru")) { fakeru = 1; fake_load(L"..\\corpus\\ru.txt"); fake_load(L"ru_words.txt"); continue; }
    if (!wcscmp(argv[a], L"-ctx")) { usectx = 1; continue; }
    if (!wcscmp(argv[a], L"-type") && a + 1 < argc) { type_text(argv[++a]); continue; }
    if (!wcscmp(argv[a], L"-p") && a + 1 < argc) {
      int* p = &g_par.db, i = 0; wchar_t* s = argv[++a];
      while (*s && i < (int)(sizeof g_par / sizeof(int))) { p[i++] = wcstol(s, &s, 10); if (*s == L',') s++; }
      continue;
    }
    for (model = 0; model < 3; model++) if (!wcsncmp(argv[a], names[model], 2)) break;
    if (model == 3 || (src = group_of_model(model)) < 0) { fwprintf(out, L"skip %s\n", argv[a]); continue; }
    if (_wfopen_s(&in, argv[a] + 3, L"r, ccs=UTF-8")) { fwprintf(out, L"cannot open %s\n", argv[a] + 3); continue; }
    fwprintf(out, L"\n=== %s\n", argv[a]);
    while (fgetws(line, 256, in)) {
      KEY k[MAXW]; int n, cur, t, neg = line[0] == L'!';
      WCHAR* w = line + neg;
      size_t len = wcslen(w);
      while (len && (w[len - 1] == L'\n' || w[len - 1] == L'\r' || w[len - 1] == L' ')) w[--len] = 0;
      if (!len || w[0] == L'#') continue;
      if ((n = to_keys(src, w, k)) < 0) { fwprintf(out, L"  cannot type: %s\n", w); continue; }
      for (cur = 0; cur < g_nlay; cur++) {
        int gain, tg, ctx = usectx && neg ? src : -1;
        t = eng_decide(k, n, cur, ctx, dict, o);
        gain = eng_gain(k, n, cur, dict, &tg);
        calls += 2;
        if (nrec < 20000 && (!neg || cur == src)) {
          CAND* c = &g_cand[cur];
          /* effective gain: minus the extra margins eng_decide adds for this case */
          rec[nrec].gain = tg >= 0 && n >= 2 ? gain - (ctx == cur ? g_par.ctxGain : 0)
                         - (c->dict == 1 || (c->common && (c->dict != 0 || c->common >= 4)) ? g_par.curValidGain : 0) : -1000000;
          rec[nrec].right = tg == src;
          rec[nrec].kind = neg || cur == src;
          rec[nrec].typo = 0;
          rec[nrec].curdict = c->dict;
          swprintf_s(rec[nrec].w, 64, L"%s as %s -> %s", w, c->t, tg >= 0 ? g_cand[tg].t : L"-");
          if (tg >= 0 && c->dict == 0 && c->c1 - c->c0 >= 3) {
            DICTQ q; q.g = cur; q.w = c->t + c->c0; q.n = c->c1 - c->c0; q.res = -1; q.typo = 1;
            dict(&q, 1);
            rec[nrec].typo = q.res != 0 ? (c->c1 - c->c0 >= 5 ? g_par.typoGain : g_par.typoGainShort) : 0;
          }
          nrec++;
        }
        if (neg) {
          if (cur != src) continue;
          negTot++;
          if (t >= 0) { negBad++; fwprintf(out, L"  NEG-SWITCHED %s -> %s\n", w, o); dump(cur); }
          continue;
        }
        tot[src][cur]++;
        if (cur == src ? t >= 0 : t != src) {
          bad[src][cur]++;
          fwprintf(out, L"  %s %s typed in %d -> %d %s\n", cur == src ? L"FALSE-SWITCH" : L"MISSED", w, cur, t, t >= 0 ? o : L"");
          dump(cur);
        }
      }
    }
    fclose(in);
  }
  QueryPerformanceCounter(&t1);

  fwprintf(out, L"\n=== summary (failures / total), rows = intended, cols = typed in\n");
  for (a = 0; a < g_nlay; a++) {
    fwprintf(out, L"  %d:", a);
    for (g = 0; g < g_nlay; g++) fwprintf(out, L"  %3d/%-4d", bad[a][g], tot[a][g]);
    fwprintf(out, L"\n");
  }
  fwprintf(out, L"  never-switch words: %d/%d switched\n", negBad, negTot);
  fwprintf(out, L"\n=== threshold sweep (minGain in 1/8 bit): missed / wrong-target / false-switch\n");
  for (a = 0; a <= 160; a += 8) {
    int miss = 0, wrong = 0, fp = 0, i;
    for (i = 0; i < nrec; i++) {
      int sw = rec[i].gain >= a && !(rec[i].typo && rec[i].gain < a + rec[i].typo);
      if (rec[i].kind) fp += sw;
      else if (!sw) miss++;
      else if (!rec[i].right) wrong++;
    }
    fwprintf(out, L"  %3d: %4d %4d %4d\n", a, miss, wrong, fp);
  }
  fwprintf(out, L"\n=== risky cases (would switch at minGain >= 0 but must not / wrong target)\n");
  for (a = 0; a < nrec; a++)
    if (rec[a].gain >= 0 && !(rec[a].typo && rec[a].gain < rec[a].typo) && (rec[a].kind || !rec[a].right))
      fwprintf(out, L"  gain=%4d typo=%d %s  %s\n", rec[a].gain, rec[a].typo, rec[a].kind ? L"FALSE-SWITCH" : L"WRONG", rec[a].w);
  fwprintf(out, L"  %.1f us per test case (incl. extra diagnostics)\n", (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / (calls ? calls : 1));
  {
    /* what the app does on Space: one eng_decide, in the right layout and in the wrong ones */
    static const WCHAR* bw[] = { L"hello", L"keyboard", L"tomorrow", L"something", L"computer", 0 };
    KEY k[MAXW]; WCHAR o2[MAXW + 1]; int i, r, n2, src = group_of_model(0), cur2; long long cnt = 0;
    QueryPerformanceCounter(&t0);
    for (r = 0; r < 200; r++)
      for (i = 0; bw[i]; i++)
        if ((n2 = to_keys(src, bw[i], k)) > 0)
          for (cur2 = 0; cur2 < g_nlay; cur2++) { eng_decide(k, n2, cur2, -1, dict, o2); cnt++; }
    QueryPerformanceCounter(&t1);
    fwprintf(out, L"  %.1f us per eng_decide (incl. spellcheck)\n", (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / (cnt ? cnt : 1));
  }
  fclose(out);
  if (ru) UnloadKeyboardLayout(ru);
  return 0;
}
