# LangFix

Fixes words typed in the wrong keyboard layout on Windows 10/11, and lets Claude fix,
rewrite or translate the text you select.

Type `יקךךם` in the Hebrew layout when you meant English, press Space, and it becomes
`hello ` and the layout switches to English. LangFix works with any installed layouts and is
tuned for English, Russian and Hebrew.

`LangFix.exe` is one file of about 490 KB. It needs no installer and no runtime.
- It already contains the 30,000 most common words of each language.
- It uses about 5 MB of memory.
- It decides on a word in well under a millisecond.

## Use

Run `LangFix.exe`. The settings window opens and an icon (two arrows) appears in the tray.
Closing the window keeps LangFix running. Click the tray icon or start the exe again to bring
the window back. "Start LangFix automatically when Windows starts" starts it quietly in the
tray.

| Key | What it does |
|---|---|
| **Space** | checks the word you just typed and fixes it if another layout clearly fits (a small hint shows what changed) |
| **Pause** or **both Shift keys** | converts the word you are typing or the last word. Right after an automatic fix it undoes the fix, and that word is left alone from then on |
| **Shift+Pause** | converts the whole selected text, e.g. a sentence typed in the wrong layout |
| **Ctrl+Alt+Enter** | Claude fixes spelling and grammar in the selection, wrong-layout words included. With nothing selected, it works on the line before the cursor |
| **Ctrl+Alt+Space** | the AI menu: fix, improve, shorten, more formal, more friendly, translate to your languages, convert layout, or ask Claude anything |
| **Esc** | cancels while Claude is working. **Ctrl+Z** in the app undoes an AI change |

Every hotkey can be changed or switched off in the settings window.

### Settings window tabs

| Tab | Settings |
|---|---|
| **General** | On/off · start with Windows · Start menu entry · fix hints · also check on Enter · update check · how eager to fix |
| **Hotkeys** | convert/undo keys · selected-text conversion · a shortcut to turn LangFix on and off |
| **Languages** | which installed layouts to switch between, and how each one is recognized |
| **Words** | *My words* (slang, names, terms to always recognize) · *Never change* · chat slang on/off |
| **AI** | Claude API key · model (Opus 5.5, Sonnet 5.5 or Haiku 5.5) · AI hotkeys · test connection |
| **Apps** | apps where automatic fixes are off, such as games and terminals. The hotkeys still work there |

It is deliberately cautious:
- Only Space triggers a fix, and Enter only if you turn that option on.
- Words with digits are never touched, and neither are classic password boxes or LangFix's own window.

## AI writing tools

AI needs a Claude API key from the [Anthropic Console](https://console.anthropic.com/settings/keys).
Paste the key on the **AI** tab. It is stored encrypted for your Windows account with DPAPI, and
the `ANTHROPIC_API_KEY` environment variable also works.

Privacy and data:
- **Only the text you select is sent, and only when you press an AI hotkey.**
- Normal typing never leaves your PC.

How a request works:
- LangFix copies the selection, sends it to `POST /v1/messages`, pastes the reply in place, and puts your clipboard back.
- Its temporary clipboard contents are kept out of clipboard history (Win+V).
- Requests use effort `low`, so short edits come back fast.
- Opus and Sonnet requests opt into server-side refusal fallbacks.

## How the fixing works

When you press Space, the word's physical keys are read through every enabled layout. Each
reading is scored using:
- a letter-trigram model,
- the 30k-word vocabulary (a top-1000 word counts most),
- the Windows spellchecker where one is installed,
- your words, the slang list,
- spelling rules, such as no Hebrew final letters in the middle of a word.

A word is replaced only when another reading is far more natural. A word the dictionary
accepts, a likely typo, or a word in the same language as the previous word all raise the bar.
An ambiguous short word such as `to`/`אם` is fixed together with the next word once that word
gives the language away.

Measured on about 1,200 test words typed in the wrong layout at the Balanced setting:
- misses: English 1.6%, Russian 2.5% (on a PC without a Russian spellchecker), Hebrew 7%;
- false switches: none, on correctly typed words or on 109 slang words, names and typos.

## Limits

- Windows does not let it see or fix typing in apps that run as administrator unless LangFix
  also runs as administrator.
- The exe is unsigned, so SmartScreen may show "Windows protected your PC". Click
  More info → Run anyway.

## Build

You need Visual Studio 2022 Build Tools (C++) and Python 3.

```
build.bat
```

The script:
- regenerates `src\model.h` when `data\freq` or `corpus` changed (a few minutes),
- builds `dist\LangFix.exe` without the C runtime,
- builds the test tools.

| Path | What it is |
|---|---|
| `src/engine.c` | layout tables, scoring and the decision |
| `src/main.c` | keyboard hook, retyping, tray, startup |
| `src/ui.c` | the settings window |
| `src/ai.c` | selected-text actions, Claude API (WinHTTP + DPAPI), hints, update check |
| `tools/build_model.py` | builds the letter model and vocabulary from `data/freq` and `corpus` |

Tests:
- `build\lftest.exe out.txt -ctx en:tests\en_words.txt ...` scores word lists offline.
- `build\e2e.exe dist\LangFix.exe out.txt` types real keystrokes with LangFix running. It covers
  33 cases, including every setting and the AI flow. Start `python tests\mock_claude.py 8765 requests.jsonl`
  first for the AI cases. Add `--edge <msedge.exe> <temp dir>` to type into an Edge window instead.

## Credits

Word frequency lists: [FrequencyWords](https://github.com/hermitdave/FrequencyWords) by Hermit Dave,
built from OpenSubtitles 2018, licensed [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/)
(see `data/freq/README.md`).
