/* LangFix detection engine: keyboard layout tables, scoring and decisions.
   No CRT, no allocations; everything lives in static tables. */
#pragma once
#include <windows.h>

#define MAXL 8      /* layout groups (layouts with identical character maps are merged) */
#define MAXW 40     /* keystrokes per word */
#define MAXH 4      /* HKLs per group */

enum { SC_NONE = 0, SC_LATIN, SC_CYR, SC_HEB, SC_GREEK, SC_ARAB, SC_OTHER };

typedef struct { BYTE vk, mods; } KEY;          /* mods: 1 = shift, 2 = caps lock */

typedef struct {
  HKL   hkl[MAXH];
  int   nhkl;
  HKL   recent;                                  /* member the user used last */
  WCHAR loc[MAXH][LOCALE_NAME_MAX_LENGTH];       /* spellchecker locales, e.g. "he-IL" */
  int   nloc;
  int   script;
  int   model;                                   /* letter model index, -1 = none */
  int   enabled;                                 /* the user lets LangFix switch to/from it */
  WCHAR map[256][4];                             /* vk x mods -> char, 0 = none */
  BYTE  bad[256];                                /* dead key / multi-char key */
} LAYOUT;

/* Per-layout view of one typed word. */
typedef struct {
  WCHAR t[MAXW + 1];
  int ok;          /* every key produced a char */
  int c0, c1;      /* core = t[c0..c1) without leading/trailing punctuation */
  int letters;     /* letters of the layout's script inside the core */
  int scriptOk;    /* core is letters of this script (plus ' and -) */
  int edgeOk;      /* stripped punctuation is ordinary ( . , ! ? quotes ... ) */
  int ng;          /* letter-model cost, 1/8 bit per char; -1 unscorable, -2 no model */
  int ngTot;       /* same, summed over the word */
  int dict;        /* 1 valid, 0 misspelled, -1 unknown */
  int common;      /* known word: 1-3 vocabulary tier (3 = top 1000), 4 slang, 5 user's word */
  int ev;          /* total evidence cost in 1/8 bits (lower = more natural) */
  int valid;       /* acceptable as a replacement */
} CAND;

/* Tunables, all in 1/8 bit. */
typedef struct {
  int db;          /* dictionary bonus, words of 4+ letters */
  int dbShort;     /* ... words of 2-3 letters */
  int dbHeb;       /* ... Hebrew (prefixes make many strings valid) */
  int dp;          /* misspelled penalty */
  int punct;       /* per stripped ordinary punctuation char */
  int punctOdd;    /* per stripped unusual punctuation char */
  int minGain;     /* switch when current costs this much more than the target */
  int minGainShort;
  int misspelledGain; /* extra gain needed when the target is also misspelled */
  int typoGain;    /* extra gain needed when the current word looks like a typo */
  int typoGainShort; /* ... for 3-4 letter words, where near misses are everywhere */
  int ctxGain;     /* extra gain needed when the previous word was in the current layout */
  int curValidGain;  /* extra gain needed when the current word is in the dictionary */
  int vocab[6];    /* bonus per CAND.common value (index 0 unused) */
  int slang;       /* 1 = recognize the built-in chat slang */
} ENGPAR;

#define MAXUSER 256
extern WCHAR g_user[MAXUSER][MAXW + 1];   /* the user's own words, lower-case */
extern int   g_nuser;

/* typo = 0: res = 1 valid word, 0 misspelled, -1 no dictionary.
   typo = 1: res = 1 if the dictionary suggests a word one edit away (a typo). */
typedef struct { int g; const WCHAR* w; int n; int res; int typo; } DICTQ;
/* Fill q[i].res for every query; return 0 if the dictionary did not answer in time. */
typedef int (*DICTFN)(DICTQ* q, int nq);

extern LAYOUT g_lay[MAXL];
extern int    g_nlay;
extern CAND   g_cand[MAXL];   /* candidates of the last decision, for diagnostics */
extern ENGPAR g_par;
extern int    g_gain;       /* gain computed by the last eng_decide */

void eng_init_layouts(void);
int  eng_add_hkl(HKL h);                        /* returns group index or -1 */
int  eng_find(HKL h);                           /* group of h, -1 if unknown; updates .recent */
BOOL eng_layouts_changed(void);
int  eng_render(int g, const KEY* k, int n, WCHAR* out, int intended);
int  eng_gain(const KEY* k, int n, int cur, DICTFN dict, int* target);
int  eng_decide(const KEY* k, int n, int cur, int ctx, DICTFN dict, WCHAR* out);
int  eng_one_edit(const WCHAR* a, int na, const WCHAR* b, int nb);

#define MAXSEL 8000   /* longest selection that is converted or sent to the AI */
/* Re-type a whole text as if its keys had been pressed in another layout (selection
   conversion). Tries every pair of enabled layouts and keeps the most natural result.
   out holds n + 1 chars. Returns the target layout group, -1 if nothing converts. */
int  eng_convert_text(const WCHAR* in, int n, WCHAR* out);
int  eng_pick(const KEY* k, int n, int shown, DICTFN dict, WCHAR* out);
int  eng_script_of(WCHAR c);
WCHAR eng_lower(WCHAR c);
