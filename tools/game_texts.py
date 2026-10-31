#!/usr/bin/env python3
"""tools/game_texts.py -- translations of the GAME's own text (menus, HUD, tutorials).

The disc has the game's text in 13 languages (findings/49). For a language it
doesn't have (Brazilian Portuguese), the port carries a translation of the
game's text banks in languages/game/<code>.po, and swaps it in at run time
(src/language/game_text_banks.cpp). The repo never holds the game's English
(rule 1): the shipped file is keyed by the bank and the string's key only:

    #, fuzzy
    msgctxt "frontend"
    msgid "213"
    msgstr "Novo jogo"

To translate or fix it, make an EDITABLE copy that has the English beside
each line, read from YOUR disc files (game/default.rcf), work on it (Poedit,
a text editor), then pack it back:

    python3 tools/game_texts.py edit pt_BR      -> user/languages/game/pt_BR.edit.po
    python3 tools/game_texts.py pack pt_BR      -> languages/game/pt_BR.po
    python3 tools/game_texts.py check pt_BR     problems a translator can fix
    python3 tools/game_texts.py status pt_BR    how much is done

The edit copy (msgctxt "frontend 213", msgid = the English) is game text: it
stays in user/ (gitignored), never in the repo. A translation must keep the
English's button pictures (the font's U+00A5-00BE: ± = A, ² = B, ...), its
"†" (a space that doesn't break a line) and its line count; `check` says
where it doesn't. Strings without letters (the name screen's keyboard keys,
digits, "*") and the language names stay as the disc has them.

The bank format: tools/text_banks.py. Which English: a key whose English
differs between packages (the Wii / PS2 copy in 97597d1b: "Nunchuk", all
capitals) takes the Xbox 360 wording.
"""
import mmap
import os
import re
import sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.abspath(__file__)) + "/.."
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import text_banks as tb  # noqa: E402
import translations as tr  # noqa: E402

GAME_DIR = os.path.join(ROOT, "languages", "game")
EDIT_DIR = os.path.join(ROOT, "user", "languages", "game")
BUTTONS = set(chr(c) for c in range(0xA5, 0xBF)) | {"†"}


def english(rcf=os.path.join(ROOT, "game", "default.rcf")):
    """OrderedDict (bank, key) -> the Xbox 360 English, from the player's disc."""
    found = {}
    with open(rcf, "rb") as f:
        d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        for name, off, size in tb.rcf_files(d):
            if not name.lower().endswith(".p3d"):
                continue
            for (bank, letter), strings in tb.banks(d, off + 12, off + size).items():
                if letter != "U":
                    continue
                for key, text in strings.items():
                    found.setdefault((bank, key), []).append(text)
    out = OrderedDict()
    for (bank, key) in sorted(found):
        texts = list(OrderedDict.fromkeys(found[(bank, key)]))
        xbox = [t for t in texts if "unchu" not in t.lower() and "wii" not in t.lower()]
        out[(bank, key)] = (xbox or texts)[0]
    return out


def translatable(text):
    return len(re.sub(r"[^A-Za-z]", "", text)) >= 2 and not re.search("[Ѐ-ӿ]", text)


def shipped_path(code):
    return os.path.join(GAME_DIR, code + ".po")


def read_shipped(code):
    path = shipped_path(code)
    if not os.path.exists(path):
        return tr.header_for(code), {}
    header, entries = tr.read_po(path)
    return header, {(e.context, int(e.english)): e for e in entries if not e.obsolete and e.english.isdigit()}


def cmd_edit(code):
    eng = english()
    _, shipped = read_shipped(code)
    entries = []
    for (bank, key), text in eng.items():
        if not translatable(text):
            continue
        e = tr.Entry()
        e.context, e.english = f"{bank} {key}", text
        old = shipped.get((bank, key))
        if old:
            e.translation, e.fuzzy, e.comments = old.translation, old.fuzzy, old.comments
        entries.append(e)
    os.makedirs(EDIT_DIR, exist_ok=True)
    path = os.path.join(EDIT_DIR, code + ".edit.po")
    tr.write_po(path, tr.header_for(code), entries)
    print(f"{path}: {len(entries)} strings, {sum(1 for e in entries if e.translation)} translated")


def cmd_pack(code):
    path = os.path.join(EDIT_DIR, code + ".edit.po")
    _, entries = tr.read_po(path)
    out = []
    for e in entries:
        if e.obsolete or not e.translation:
            continue
        bank, key = e.context.split(" ")
        s = tr.Entry()
        s.context, s.english, s.translation, s.fuzzy, s.comments = bank, key, e.translation, e.fuzzy, e.comments
        out.append(s)
    out.sort(key=lambda s: (s.context, int(s.english)))
    header, _ = read_shipped(code)
    os.makedirs(GAME_DIR, exist_ok=True)
    tr.write_po(shipped_path(code), header, out)
    print(f"{shipped_path(code)}: {len(out)} strings")


def problems(english_text, translation):
    """What a translation breaks (empty = fine)."""
    out = []
    pictures = lambda s: sorted(c for c in s if c in BUTTONS)
    if pictures(english_text) != pictures(translation):
        out.append(f"button pictures / † {''.join(pictures(english_text))!r} vs {''.join(pictures(translation))!r}")
    if english_text.count("\n") != translation.count("\n"):
        out.append(f"{english_text.count(chr(10)) + 1} lines in English, {translation.count(chr(10)) + 1} here")
    bad = sorted(set(c for c in translation if c not in tr.FONT and c not in english_text))
    if bad:
        out.append(f"the game's font has no {' '.join(bad)}")
    if tr.placeholders(english_text) != tr.placeholders(translation):
        out.append("placeholders differ")
    return out


def cmd_check(code):
    eng = english()
    _, shipped = read_shipped(code)
    count = 0
    for (bank, key), e in shipped.items():
        text = eng.get((bank, key))
        if text is None:
            print(f"{bank} {key}: not a string of this disc")
            count += 1
            continue
        for p in problems(text, e.translation):
            print(f"{bank} {key}: {p}: {e.translation[:60]!r}")
            count += 1
    print("no problems" if not count else f"{count} problems")
    return count


def cmd_status(code):
    eng = english()
    _, shipped = read_shipped(code)
    todo = [k for k, t in eng.items() if translatable(t)]
    done = [k for k in todo if k in shipped]
    checked = [k for k in done if not shipped[k].fuzzy]
    print(f"{code}: {len(done)} of {len(todo)} strings translated, {len(checked)} checked by a speaker")


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("edit", "pack", "check", "status"):
        print(__doc__)
        return 1
    cmd, code = sys.argv[1], sys.argv[2]
    if cmd == "edit":
        cmd_edit(code)
    elif cmd == "pack":
        cmd_pack(code)
    elif cmd == "check":
        return 1 if cmd_check(code) else 0
    else:
        cmd_status(code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
