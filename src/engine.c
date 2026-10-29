/* LangFix detection engine. See engine.h. */
#include "engine.h"
#include "model.h"

LAYOUT g_lay[MAXL];
int    g_nlay;
CAND   g_cand[MAXL];

/* db, dbShort, dbHeb, dp, punct, punctOdd, minGain, minGainShort, misspelledGain,
   typoGain, typoGainShort, ctxGain, curValidGain,
   vocab bonus { -, rank 5k-30k, rank 1k-5k, top 1k, slang, user word }, slang on */
ENGPAR g_par = { 96, 0, 16, 32, 40, 80, 64, 64, 48, 32, 0, 48, 24, { 0, 32, 64, 96, 56, 96 }, 1 };
int    g_gain;
WCHAR  g_user[MAXUSER][MAXW + 1];
int    g_nuser;

static HKL s_list[32];
static int s_nlist;

#define NG_GOOD  46        /* a dictionary-less target must average below 5.75 bits/char */
#define NOPE     (1 << 20) /* evidence of text that cannot be a word in its layout */
#define AMBIG    16        /* two targets closer than 2 bits: do nothing */

typedef struct {
  int V;
  WCHAR base;
  int count;
  const unsigned char* t;
  const wchar_t* const* shortw;
  const unsigned char* voc;
  const unsigned int* blk;
  int nblk, nvoc;
  const wchar_t* const* slang;
} NGMODEL;

#define VOCAB(x) VOC_##x, VOCB_##x, sizeof VOCB_##x / sizeof VOCB_##x[0], sizeof VOC_##x, SLANG_##x
static const NGMODEL MODELS[3] = {
  { 27, L'a', 26, NG_EN, SHORT_EN, VOCAB(EN) },
  { 33, 0x430, 32, NG_RU, SHORT_RU, VOCAB(RU) },
  { 28, 0x5D0, 27, NG_HE, SHORT_HE, VOCAB(HE) },
};

int eng_script_of(WCHAR c) {
  if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return SC_LATIN;
  if (c < 0xC0) return SC_NONE;
  if (c <= 0x24F) return (c == 0xD7 || c == 0xF7) ? SC_NONE : SC_LATIN;
  if (c >= 0x370 && c <= 0x3FF) return SC_GREEK;
  if (c >= 0x400 && c <= 0x52F) return SC_CYR;
  if (c >= 0x5D0 && c <= 0x5EA) return SC_HEB;
  if (c >= 0x620 && c <= 0x64A) return SC_ARAB;
  return IsCharAlphaW(c) ? SC_OTHER : SC_NONE;
}

WCHAR eng_lower(WCHAR c) {
  if (c >= L'A' && c <= L'Z') return (WCHAR)(c + 32);
  if (c >= 0x410 && c <= 0x42F) return (WCHAR)(c + 32);
  if (c == 0x401 || c == 0x451) return 0x435;           /* ё -> е */
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (WCHAR)(c + 32);
  return c;
}

/* ---------- layout tables ---------- */

static void build_map(LAYOUT* L, HKL h) {
  BYTE ks[256];
  WCHAR buf[8];
  int vk, m, i, r;
  UINT sc;
  for (vk = 1; vk < 255; vk++) {
    if (vk <= VK_XBUTTON2 || vk == VK_BACK || vk == VK_TAB || vk == VK_RETURN ||
        vk == VK_ESCAPE || vk == VK_SPACE || vk == VK_PACKET)
      continue;
    sc = MapVirtualKeyExW((UINT)vk, MAPVK_VK_TO_VSC, h);
    if (!sc) continue;
    for (m = 0; m < 4; m++) {
      for (i = 0; i < 256; i++) ks[i] = 0;
      if (m & 1) ks[VK_SHIFT] = ks[VK_LSHIFT] = 0x80;
      if (m & 2) ks[VK_CAPITAL] = 0x01;
      /* flag 4: do not touch the kernel keyboard state (dead keys) */
      r = ToUnicodeEx((UINT)vk, sc, ks, buf, 8, 4, h);
      if (r == 1 && buf[0] >= 0x20 && buf[0] != 0x7F) L->map[vk][m] = buf[0];
      else if (r < 0 || r > 1) L->bad[vk] = 1;
    }
  }
}

static int same_map(const LAYOUT* a, const LAYOUT* b) {
  int vk, m;
  for (vk = 0; vk < 256; vk++)
    for (m = 0; m < 4; m++)
      if (a->map[vk][m] != b->map[vk][m]) return 0;
  return 1;
}

static int str_ieq(const WCHAR* a, const WCHAR* b) {
  return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

int eng_find(HKL h) {
  int g, i;
  for (g = 0; g < g_nlay; g++)
    for (i = 0; i < g_lay[g].nhkl; i++)
      if (g_lay[g].hkl[i] == h) { g_lay[g].recent = h; return g; }
  return -1;
}

int eng_add_hkl(HKL h) {
  static LAYOUT tmp;
  LAYOUT* L;
  WCHAR loc[LOCALE_NAME_MAX_LENGTH];
  BYTE* p;
  int g, i, cnt[SC_OTHER + 1];
  LANGID lang = LOWORD((UINT_PTR)h);

  if (!h) return -1;
  g = eng_find(h);
  if (g >= 0) return g;

  for (p = (BYTE*)&tmp, i = 0; i < (int)sizeof tmp; i++) p[i] = 0;
  build_map(&tmp, h);
  for (g = 0; g < g_nlay; g++)
    if (same_map(&g_lay[g], &tmp)) break;
  if (g == g_nlay) {
    if (g_nlay == MAXL) return -1;
    g_lay[g] = tmp;
    L = &g_lay[g];
    L->model = -1;
    L->enabled = 1;
    for (i = 0; i <= SC_OTHER; i++) cnt[i] = 0;
    for (i = 'A'; i <= 'Z'; i++) cnt[eng_script_of(L->map[i][0])]++;
    L->script = SC_LATIN;
    for (i = 1; i <= SC_OTHER; i++)
      if (cnt[i] > cnt[L->script]) L->script = i;
    g_nlay++;
  }
  L = &g_lay[g];
  if (L->nhkl < MAXH) L->hkl[L->nhkl++] = h;
  if (!L->recent) L->recent = h;

  if (L->model < 0) {
    if (L->script == SC_HEB) L->model = 2;
    else if (L->script == SC_LATIN && PRIMARYLANGID(lang) == LANG_ENGLISH) L->model = 0;
    else if (L->script == SC_CYR && PRIMARYLANGID(lang) == LANG_RUSSIAN) L->model = 1;
  }

  if (L->nloc < MAXH && LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), loc, LOCALE_NAME_MAX_LENGTH, 0)) {
    for (i = 0; i < L->nloc; i++)
      if (str_ieq(L->loc[i], loc)) break;
    if (i == L->nloc) {
      for (i = 0; (L->loc[L->nloc][i] = loc[i]) != 0; i++) {}
      L->nloc++;
    }
  }
  return g;
}

void eng_init_layouts(void) {
  int i;
  g_nlay = 0;
  s_nlist = GetKeyboardLayoutList(32, s_list);
  for (i = 0; i < s_nlist; i++) eng_add_hkl(s_list[i]);
}

BOOL eng_layouts_changed(void) {
  HKL now[32];
  int n = GetKeyboardLayoutList(32, now), i;
  if (n != s_nlist) return TRUE;
  for (i = 0; i < n; i++)
    if (now[i] != s_list[i]) return TRUE;
  return FALSE;
}

/* ---------- rendering & scoring ---------- */

/* intended: the user meant this layout, so Shift+letter keys that yield
   another script (Hebrew layout gives Latin capitals) map to the plain letter. */
int eng_render(int g, const KEY* k, int n, WCHAR* out, int intended) {
  const LAYOUT* L = &g_lay[g];
  int i, s;
  WCHAR c, b;
  for (i = 0; i < n; i++) {
    c = L->map[k[i].vk][k[i].mods & 3];
    if (intended && c) {
      s = eng_script_of(c);
      if (s && s != L->script) {
        b = L->map[k[i].vk][0];
        if (b && eng_script_of(b) == L->script) c = b;
      }
    }
    if (!c || L->bad[k[i].vk]) { out[i] = 0; return 0; }
    out[i] = c;
  }
  out[n] = 0;
  return 1;
}

static int is_joiner(WCHAR c) { return c == L'\'' || c == L'-' || c == 0x2019; }

/* Letter-trigram cost of a word (parts split at ' and -). Returns the
   average per predicted symbol, -1 if a char is outside the model. */
static int ng_score(int m, const WCHAR* s, int n, int* total) {
  const NGMODEL* M = &MODELS[m];
  int V = M->V, a = 0, b = 0, sum = 0, cnt = 0, inword = 0, i, c;
  WCHAR ch;
  for (i = 0; i <= n; i++) {
    if (i == n || is_joiner(s[i])) {
      if (inword) { sum += M->t[(a * V + b) * V]; cnt++; }
      a = b = 0;
      inword = 0;
      continue;
    }
    ch = eng_lower(s[i]);
    if (ch < M->base || ch >= M->base + M->count) return -1;
    c = ch - M->base + 1;
    sum += M->t[(a * V + b) * V + c];
    cnt++;
    a = b;
    b = c;
    inword = 1;
  }
  *total = sum;
  return cnt ? sum / cnt : -1;
}

/* Spelling rules the letter model learns too weakly. */
static int rule_cost(int script, const WCHAR* s, int n) {
  int i, e = 0, last;
  WCHAR c;
  if (script == SC_HEB) {
    for (i = 0; i < n; i++) {
      c = s[i];
      last = i == n - 1 || eng_script_of(s[i + 1]) != SC_HEB;
      if (!last && (c == 0x5DA || c == 0x5DD || c == 0x5DF || c == 0x5E3 || c == 0x5E5)) e += 48;
      if (last && i > 0 && (c == 0x5DB || c == 0x5DE || c == 0x5E0 || c == 0x5E4 || c == 0x5E6)) e += 24;
    }
  } else if (script == SC_CYR && n) {
    c = eng_lower(s[0]);
    if (c == 0x44A || c == 0x44B || c == 0x44C) e += 48;   /* ъ ы ь never start a word */
  }
  return e;
}

static int is_short(int m, const WCHAR* s) {
  const wchar_t* const* w;
  if (m < 0) return 0;
  for (w = MODELS[m].shortw; *w; w++)
    if (eng_lower(s[0]) == (*w)[0] && eng_lower(s[1]) == (*w)[1] && !(*w)[2]) return 1;
  return 0;
}

static int cmp_codes(const BYTE* a, int na, const BYTE* b, int nb) {
  int i;
  for (i = 0; i < na && i < nb; i++)
    if (a[i] != b[i]) return (int)a[i] - (int)b[i];
  return na - nb;
}

/* Decode one front-coded entry into w (which holds the previous word). */
static const BYTE* voc_next(const BYTE* p, BYTE* w, int* wl, int* tier) {
  int n = *p >> 2;
  *tier = *p++ & 3;
  do w[n++] = *p & 0x7F; while (!(*p++ & 0x80) && n < 63);
  *wl = n;
  return p;
}

/* Vocabulary tier of s[0..n) (any case): 3 top 1000 words ... 1 top 30000, 0 unknown. */
static int vocab_tier(int m, const WCHAR* s, int n) {
  const NGMODEL* M = &MODELS[m];
  BYTE q[MAXW], w[64];
  const BYTE *p, *end;
  int i, lo = 0, hi = M->nblk - 1, mid, b = 0, wl, tier, d;
  WCHAR c;
  if (n < 2 || n > MAXW) return 0;
  for (i = 0; i < n; i++) {
    c = eng_lower(s[i]);
    if (c < M->base || c >= M->base + M->count) return 0;
    q[i] = (BYTE)(c - M->base + 1);
  }
  while (lo <= hi) {                    /* last block whose first word <= q */
    mid = (lo + hi) / 2;
    voc_next(M->voc + M->blk[mid], w, &wl, &tier);
    if (cmp_codes(w, wl, q, n) <= 0) { b = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  p = M->voc + M->blk[b];
  end = b + 1 < M->nblk ? M->voc + M->blk[b + 1] : M->voc + M->nvoc;
  while (p < end) {
    p = voc_next(p, w, &wl, &tier);
    d = cmp_codes(w, wl, q, n);
    if (!d) return tier;
    if (d > 0) break;
  }
  return 0;
}

static int word_eq(const WCHAR* s, int n, const wchar_t* w) {
  int i;
  for (i = 0; i < n; i++)
    if (eng_lower(s[i]) != eng_lower(w[i])) return 0;
  return !w[n];
}

/* CAND.common for s[0..n): user's word, vocabulary tier or slang (the stronger). */
static int known_word(int m, const WCHAR* s, int n) {
  const wchar_t* const* sl;
  int i, k = 0;
  for (i = 0; i < g_nuser; i++)
    if (word_eq(s, n, g_user[i])) return 5;
  if (m < 0) return 0;
  k = vocab_tier(m, s, n);
  if (g_par.slang && g_par.vocab[4] > g_par.vocab[k])
    for (sl = MODELS[m].slang; *sl; sl++)
      if (word_eq(s, n, *sl)) return 4;
  return k;
}

static int edge_ok(WCHAR c, int leading) {
  static const WCHAR lead[] = L"([{\"'\x00AB\x201C\x201E\x2018";
  static const WCHAR trail[] = L".,!?:;)]}\"'\x00BB\x201D\x2019\x2026";
  const WCHAR* p;
  for (p = leading ? lead : trail; *p; p++)
    if (*p == c) return 1;
  return 0;
}

static void analyze(int g, const KEY* k, int n, CAND* C, int intended) {
  const LAYOUT* L = &g_lay[g];
  int i;
  WCHAR c;
  C->c0 = C->c1 = C->letters = C->scriptOk = C->edgeOk = C->ngTot = C->valid = C->common = 0;
  C->ng = -2;
  C->dict = -1;
  C->ev = NOPE;
  C->ok = eng_render(g, k, n, C->t, intended);
  if (!C->ok) return;

  for (i = 0; i < n && !eng_script_of(C->t[i]); i++) {}
  C->c0 = i;
  for (i = n; i > C->c0 && !eng_script_of(C->t[i - 1]); i--) {}
  C->c1 = i;

  C->edgeOk = 1;
  for (i = 0; i < C->c0; i++) if (!edge_ok(C->t[i], 1)) C->edgeOk = 0;
  for (i = C->c1; i < n; i++) if (!edge_ok(C->t[i], 0)) C->edgeOk = 0;

  C->scriptOk = C->c1 > C->c0;
  for (i = C->c0; i < C->c1; i++) {
    c = C->t[i];
    if (eng_script_of(c) == L->script) C->letters++;
    else if (!is_joiner(c) || is_joiner(C->t[i + 1])) C->scriptOk = 0;
  }
  if (L->model >= 0)
    C->ng = C->scriptOk ? ng_score(L->model, C->t + C->c0, C->c1 - C->c0, &C->ngTot) : -1;
  if (C->scriptOk && C->letters == C->c1 - C->c0)
    C->common = known_word(L->model, C->t + C->c0, C->letters);
}

/* Total cost of reading the keys as this candidate, after the dictionary answered. */
static void evidence(int g, CAND* C, int n) {
  const LAYOUT* L = &g_lay[g];
  int len = C->c1 - C->c0, e;
  if (!C->ok || !C->scriptOk) { C->ev = NOPE; return; }
  if (C->ng >= 0) e = C->ngTot;
  else e = (C->dict == 1 ? 24 : C->dict == 0 ? 56 : 40) * (len + 1);
  e += rule_cost(L->script, C->t + C->c0, len);
  if (C->dict == 1) e -= len < 4 ? g_par.dbShort : L->script == SC_HEB ? g_par.dbHeb : g_par.db;
  else if (C->dict == 0) e += g_par.dp;
  /* the frequency lists hold some subtitle junk ("lf" for "If"): when a real
     dictionary rejects the word, the list only counts half */
  e -= C->dict == 0 && C->common < 4 ? g_par.vocab[C->common] / 2 : g_par.vocab[C->common];
  e += (n - len) * (C->edgeOk ? g_par.punct : g_par.punctOdd);
  C->ev = e;
}

static int ask_dict(DICTFN dict) {
  DICTQ q[MAXL];
  int nq = 0, g, i;
  for (g = 0; g < g_nlay; g++) {
    CAND* C = &g_cand[g];
    if (!C->ok || !C->scriptOk || C->c1 - C->c0 < 2 || !g_lay[g].nloc) continue;
    q[nq].g = g;
    q[nq].w = C->t + C->c0;
    q[nq].n = C->c1 - C->c0;
    q[nq].res = -1;
    q[nq].typo = 0;
    nq++;
  }
  if (!nq) return 1;
  if (!dict(q, nq)) return 0;
  for (i = 0; i < nq; i++) g_cand[q[i].g].dict = q[i].res;
  return 1;
}

/* Score every layout's reading of the keys. Returns how much more the current
   reading costs than the best acceptable other reading (1/8 bits), target in *target. */
int eng_gain(const KEY* k, int n, int cur, DICTFN dict, int* target) {
  CAND *c, *T;
  int g, i, len, shortw, gain, best = -1, bestGain = -NOPE, second = -NOPE;

  *target = -1;
  if (n < 1 || n > MAXW || cur < 0 || cur >= g_nlay || !g_lay[cur].enabled) return -NOPE;
  for (g = 0; g < g_nlay; g++) analyze(g, k, n, &g_cand[g], g != cur);
  c = &g_cand[cur];
  if (!c->ok) return -NOPE;
  for (i = 0; i < n; i++)
    if (c->t[i] >= L'0' && c->t[i] <= L'9') return -NOPE;   /* numbers, codes, passwords */
  if (!ask_dict(dict)) return -NOPE;
  for (g = 0; g < g_nlay; g++) evidence(g, &g_cand[g], n);

  for (g = 0; g < g_nlay; g++) {
    T = &g_cand[g];
    if (g == cur || !g_lay[g].enabled || !T->ok || !T->scriptOk || !T->edgeOk) continue;
    len = T->c1 - T->c0;
    if (len < 2) continue;
    shortw = len == 2;
    if (shortw && T->common != 5 && !is_short(g_lay[g].model, T->t + T->c0)) continue;
    gain = c->ev - T->ev;
    if (T->dict == 1 || T->common) T->valid = 1;
    else if (T->dict == 0) T->valid = len >= 5 && T->ng >= 0 && T->ng <= NG_GOOD && gain >= g_par.minGain + g_par.misspelledGain;
    else T->valid = shortw || (T->ng >= 0 && len >= 3 &&
                               (T->ng <= NG_GOOD || (T->ng <= NG_GOOD + 12 && gain >= 2 * g_par.minGain)));
    if (!T->valid) continue;
    if (shortw) gain -= g_par.minGainShort - g_par.minGain;   /* 2-letter words need more */
    if (gain > bestGain) { second = bestGain; bestGain = gain; best = g; }
    else if (gain > second) second = gain;
  }
  if (best >= 0 && bestGain - second < AMBIG) return -NOPE;
  *target = best;
  return bestGain;
}

/* Returns the layout group the word was meant for (text in out), or -1.
   ctx: layout of the previous word in this text, -1 if unknown. */
int eng_decide(const KEY* k, int n, int cur, int ctx, DICTFN dict, WCHAR* out) {
  int t, i, gain, len, need = g_par.minGain;
  CAND* c;
  DICTQ q;
  g_gain = -NOPE;
  if (n < 2) return -1;
  g_gain = gain = eng_gain(k, n, cur, dict, &t);
  c = &g_cand[cur];
  len = c->c1 - c->c0;
  if (ctx == cur) need += g_par.ctxGain;
  if (c->dict == 1 || (c->common && (c->dict != 0 || c->common >= 4))) need += g_par.curValidGain;
  if (t < 0 || gain < need) return -1;
  if (c->dict == 0 && len >= 3 && gain < need + (len >= 5 ? g_par.typoGain : g_par.typoGainShort)) {
    q.g = cur; q.w = c->t + c->c0; q.n = c->c1 - c->c0; q.res = -1; q.typo = 1;
    if (!dict(&q, 1) || q.res != 0) return -1;     /* a typo of a real word, or no answer */
  }
  for (i = 0; i <= n; i++) out[i] = g_cand[t].t[i];
  return t;
}

/* Damerau (adjacent swap) edit distance <= 1, case-insensitive. */
int eng_one_edit(const WCHAR* a, int na, const WCHAR* b, int nb) {
  int i = 0, j;
  if (na - nb > 1 || nb - na > 1) return 0;
  while (i < na && i < nb && eng_lower(a[i]) == eng_lower(b[i])) i++;
  if (i == na && i == nb) return 1;
  if (na == nb) {
    if (i + 1 < na && eng_lower(a[i]) == eng_lower(b[i + 1]) && eng_lower(a[i + 1]) == eng_lower(b[i])) i += 2;
    else i++;
    for (j = i; j < na; j++) if (eng_lower(a[j]) != eng_lower(b[j])) return 0;
    return 1;
  }
  if (na > nb) { for (j = i; j < nb; j++) if (eng_lower(a[j + 1]) != eng_lower(b[j])) return 0; }
  else { for (j = i; j < na; j++) if (eng_lower(a[j]) != eng_lower(b[j + 1])) return 0; }
  return 1;
}

/* Manual conversion: the most natural other layout. */
int eng_pick(const KEY* k, int n, int shown, DICTFN dict, WCHAR* out) {
  CAND* T;
  int g, i, score, best = -1, bestScore = 0x7FFFFFFF;
  if (n < 1 || n > MAXW) return -1;
  for (g = 0; g < g_nlay; g++) analyze(g, k, n, &g_cand[g], 1);
  ask_dict(dict);
  for (g = 0; g < g_nlay; g++) {
    T = &g_cand[g];
    evidence(g, T, n);
    if (g == shown || !T->ok || !g_lay[g].enabled) continue;
    score = T->scriptOk ? T->ev : NOPE + (T->edgeOk ? 0 : 1000);
    if (score < bestScore) { bestScore = score; best = g; }
  }
  if (best < 0) return -1;
  for (i = 0; i <= n; i++) out[i] = g_cand[best].t[i];
  return best;
}

/* ---------- whole-text conversion ---------- */

/* The key that types c in layout g (plain keys first), 0 if none. */
static int find_key(int g, WCHAR c, KEY* k) {
  const LAYOUT* L = &g_lay[g];
  int vk, m;
  for (m = 0; m < 4; m++)
    for (vk = 1; vk < 255; vk++)
      if (L->map[vk][m] == c && !L->bad[vk]) {
        k->vk = (BYTE)vk;
        k->mods = (BYTE)m;
        return 1;
      }
  return 0;
}

/* How unnatural a whole text reads in layout g's language (lower is better). */
static int text_cost(int g, const WCHAR* s, int n) {
  const LAYOUT* L = &g_lay[g];
  int i = 0, j, tot = 0, ngt, sc;
  while (i < n) {
    sc = eng_script_of(s[i]);
    if (!sc) { i++; continue; }
    if (sc != L->script) { tot += 64; i++; continue; }      /* a letter of another script */
    for (j = i; j < n && eng_script_of(s[j]) == L->script; j++) {}
    if (L->model >= 0 && ng_score(L->model, s + i, j - i, &ngt) >= 0)
      tot += ngt - g_par.vocab[known_word(L->model, s + i, j - i)];
    else
      tot += 40 * (j - i + 1);
    tot += rule_cost(L->script, s + i, j - i);
    i = j;
  }
  return tot;
}

int eng_convert_text(const WCHAR* in, int n, WCHAR* out) {
  static WCHAR work[MAXSEL + 1];
  WCHAR one[2];
  KEY k;
  int src, t, i, changed, cost, best = -1, bestCost = 0x7FFFFFFF;
  if (n < 1 || n > MAXSEL) return -1;
  for (src = 0; src < g_nlay; src++) {
    if (!g_lay[src].enabled) continue;
    for (t = 0; t < g_nlay; t++) {
      if (t == src || !g_lay[t].enabled) continue;
      for (changed = 0, i = 0; i < n; i++) {
        work[i] = in[i];
        /* letters this layout can't type (already right, digits, spaces) stay as they are */
        if (find_key(src, in[i], &k) && eng_render(t, &k, 1, one, 1)) work[i] = one[0];
        changed |= work[i] != in[i];
      }
      if (!changed) continue;
      cost = text_cost(t, work, n);
      if (cost < bestCost) {
        bestCost = cost;
        best = t;
        for (i = 0; i < n; i++) out[i] = work[i];
        out[n] = 0;
      }
    }
  }
  return best;
}
