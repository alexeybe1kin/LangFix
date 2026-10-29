"""Build the language data for en/ru/he into src/model.h

Inputs
  data/freq/<lang>_50k.txt   word frequency lists (FrequencyWords by Hermit Dave,
                             OpenSubtitles 2018, CC BY-SA 4.0)
  corpus/<lang>.txt          hand-written everyday text
  corpus/slang_<lang>.txt    chat slang (a separate, switchable list)

Outputs per language
  NG_xx      letter-trigram costs: -log2 P(c | a b) in 1/8 bit (uint8), index 0 = word boundary.
             Witten-Bell smoothing; trained on the vocabulary weighted by log frequency
             plus the corpus.
  VOC_xx     the TOP most frequent words, sorted, front-coded in blocks of BLOCK words:
               header byte = shared prefix length (0..15) << 2 | tier (1..3)
               then the remaining letters as alphabet codes, the last one | 0x80
  VOCB_xx    byte offset of each block
  SHORT_xx   2-letter words that may be produced by a fix
  SLANG_xx   chat slang
"""
import math, os, re, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOP = 30000
BLOCK = 32
TIERS = ((1000, 3), (5000, 2), (TOP, 1))     # rank below -> tier

LANGS = {
    # name: (first letter code point, letter count)
    "en": (ord("a"), 26),
    "ru": (0x430, 32),   # а..я, ё folded into е
    "he": (0x5D0, 27),   # א..ת including final forms
}

SHORT = {
    "en": "am an as at be by do go he hi if in is it me my no of oh ok on or so to up us we",
    "ru": "да не но на он по из за до от во ко со то же ли бы мы вы их ее ты ну ой ах эх уж ок вот как так где кто что все это там тут",
    "he": "כן לא זה זו מה מי של על אל עם גם את אם כי יש אז רק או פה שם הם הן די לי לך לו לה בו בה כל אך עד בא אח אב יד",
}


def norm(lang, text):
    text = text.lower()
    if lang == "ru":
        text = text.replace("ё", "е")
    return text


def letter_class(lang):
    base, n = LANGS[lang]
    return "[%s-%s]" % (re.escape(chr(base)), re.escape(chr(base + n - 1)))


def words(lang, text):
    return re.findall(letter_class(lang) + "+", norm(lang, text))


def read(path):
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8") as f:
        return "".join(l for l in f if not l.startswith("#"))


def load_freq(lang):
    """[(word, count)] most frequent first, letters of the language only."""
    path = os.path.join(ROOT, "data", "freq", lang + "_50k.txt")
    if not os.path.exists(path):
        return []
    whole = re.compile(letter_class(lang) + "{2,40}$")
    counts = collections.Counter()
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2:
                continue
            w = norm(lang, parts[0])
            if whole.match(w):
                counts[w] += int(parts[1])
    return counts.most_common(TOP)


def train(lang, weighted):
    """weighted: iterable of (word, weight). Returns the uint8 cost table."""
    base, n = LANGS[lang]
    V = n + 1
    c3, c2, c1 = collections.Counter(), collections.Counter(), collections.Counter()
    for w, wt in weighted:
        seq = [0, 0] + [ord(ch) - base + 1 for ch in w] + [0]
        for i in range(2, len(seq)):
            a, b, c = seq[i - 2], seq[i - 1], seq[i]
            c3[(a, b, c)] += wt
            c2[(b, c)] += wt
            c1[c] += wt
    N = sum(c1.values())
    ctx2, typ2, ctx3, typ3 = (collections.Counter() for _ in range(4))
    for (b, c), k in c2.items():
        ctx2[b] += k
        typ2[b] += 1
    for (a, b, c), k in c3.items():
        ctx3[(a, b)] += k
        typ3[(a, b)] += 1

    def p1(c):
        return (c1[c] + 1) / (N + V)

    def p2(b, c):
        t, k = typ2[b], ctx2[b]
        return p1(c) if k == 0 else (c2[(b, c)] + t * p1(c)) / (k + t)

    def p3(a, b, c):
        t, k = typ3[(a, b)], ctx3[(a, b)]
        return p2(b, c) if k == 0 else (c3[(a, b, c)] + t * p2(b, c)) / (k + t)

    table = bytearray(V * V * V)
    for a in range(V):
        for b in range(V):
            for c in range(V):
                if c == 0 and b == 0:
                    cost = 255  # empty word never happens
                else:
                    cost = min(255, int(round(-math.log2(p3(a, b, c)) * 8)))
                table[(a * V + b) * V + c] = cost
    return table


def encode_vocab(lang, tiered):
    """tiered: {word: tier}. Returns (blob, block offsets)."""
    base, _ = LANGS[lang]
    items = sorted((tuple(ord(ch) - base + 1 for ch in w), t) for w, t in tiered.items())
    blob, offs, prev = bytearray(), [], ()
    for i, (codes, tier) in enumerate(items):
        if i % BLOCK == 0:
            offs.append(len(blob))
            pre = 0
        else:
            pre = 0
            while pre < min(len(prev), len(codes), 15) and prev[pre] == codes[pre]:
                pre += 1
            if pre == len(codes):      # a word never equals a prefix of the previous one
                pre -= 1
        blob.append(pre << 2 | tier)
        rest = codes[pre:]
        blob.extend(rest[:-1])
        blob.append(rest[-1] | 0x80)
        prev = codes
    return blob, offs


def c_wstr(s):
    return 'L"' + "".join("\\x%04x" % ord(ch) if ord(ch) > 127 else ch for ch in s) + '"'


def c_bytes(name, data, ctype="unsigned char", per=40):
    out = ["static const %s %s[%d] = {" % (ctype, name, len(data))]
    for i in range(0, len(data), per):
        out.append("  " + ",".join(str(x) for x in data[i:i + per]) + ",")
    out.append("};")
    return out


def up_to_date(path):
    """model.h is newer than this script and every input file."""
    if not os.path.exists(path):
        return False
    inputs = [os.path.abspath(__file__)]
    for d in ("corpus", os.path.join("data", "freq")):
        full = os.path.join(ROOT, d)
        if os.path.isdir(full):
            inputs += [os.path.join(full, f) for f in os.listdir(full)]
    return all(os.path.getmtime(f) <= os.path.getmtime(path) for f in inputs)


def main():
    if up_to_date(os.path.join(ROOT, "src", "model.h")) and "--force" not in sys.argv:
        print("model.h is up to date")
        return
    out = ["/* generated by tools/build_model.py - do not edit */", "#pragma once", ""]
    total = 0
    for lang in LANGS:
        freq = load_freq(lang)
        corpus = words(lang, read(os.path.join(ROOT, "corpus", lang + ".txt")))
        slang = sorted(set(words(lang, read(os.path.join(ROOT, "corpus", "slang_%s.txt" % lang)))))

        cmin = freq[-1][1] if freq else 1
        weighted = [(w, 1.0) for w in corpus] + [(w, 1.0) for w in slang]
        weighted += [(w, 1.0 + math.log2(c / cmin)) for w, c in freq]
        table = train(lang, weighted)

        tiered = {}
        for rank, (w, _) in enumerate(freq):
            tiered[w] = next(t for limit, t in TIERS if rank < limit)
        for w in corpus:
            if 2 <= len(w) <= 40:
                tiered.setdefault(w, 1)
        blob, offs = encode_vocab(lang, tiered)
        total += len(table) + len(blob) + 4 * len(offs)

        L = lang.upper()
        out.append("/* %s: %d vocabulary words (%d from frequency list), %d slang, %d vocab bytes */"
                   % (lang, len(tiered), len(freq), len(slang), len(blob)))
        out += c_bytes("NG_" + L, table, per=48)
        out += c_bytes("VOC_" + L, blob)
        out += c_bytes("VOCB_" + L, offs, "unsigned int", 16)
        out.append("static const wchar_t* const SHORT_%s[] = { %s, 0 };" % (L, ", ".join(c_wstr(w) for w in SHORT[lang].split())))
        out.append("static const wchar_t* const SLANG_%s[] = { %s, 0 };" % (L, ", ".join(c_wstr(w) for w in slang)))
        out.append("")
        print("%s: %d words, %d slang, %d KB" % (lang, len(tiered), len(slang), (len(blob) + 4 * len(offs)) // 1024))
    path = os.path.join(ROOT, "src", "model.h")
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s (%d KB of data)" % (path, total // 1024))


if __name__ == "__main__":
    main()
