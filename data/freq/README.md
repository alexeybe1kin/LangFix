# Word frequency lists

`en_50k.txt`, `ru_50k.txt` and `he_50k.txt` are copied unchanged from
[FrequencyWords](https://github.com/hermitdave/FrequencyWords) by Hermit Dave
(`content/2018/<lang>/<lang>_50k.txt`). The lists are built from the OpenSubtitles 2018 corpus
and licensed [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

`tools/build_model.py` derives LangFix's vocabulary and letter model (`src/model.h`) from them.
That derived data falls under the same license.
