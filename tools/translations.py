#!/usr/bin/env python3
"""tools/translations.py -- keeps the port's translation files in step with the code.

The port's own texts are written in English in the C++ code, wrapped in
tr(u"..."), trc("context", u"...") or trn(u"...") (src/language/texts.h).
This tool:

  extract   finds every such text (also trcn("context", u"...")) in src/ and writes languages/template.pot
            (the list translators work from; never edit it by hand)
  update    extract, then brings every languages/<code>.po up to date: new
            texts added (empty), changed references/notes refreshed, texts the
            code no longer has kept at the end as "#~" (obsolete) so nothing
            translated is lost
  check     problems a translator can fix: untranslated, {0} placeholders
            that don't match the English, characters the game's font doesn't
            have (it would draw nothing there), line counts that differ
  status    the progress table in languages/README.md (between the
            <!-- status --> markers)
  new CODE  starts languages/CODE.po from the template (e.g. "new pl")

Typical use after changing texts in the code:
    python3 tools/translations.py update && python3 tools/translations.py status

The .po format is GNU gettext's, so any translation tool can open the files
(Poedit, Weblate, a text editor). Machine drafts are marked "#, fuzzy" until
a person who speaks the language has checked them; removing that line = checked.

No third-party Python modules: the .po reading and writing is done here.
"""
import os
import re
import sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
LANG_DIR = os.path.join(ROOT, "languages")
TEMPLATE = os.path.join(LANG_DIR, "template.pot")
README = os.path.join(LANG_DIR, "README.md")

# Each language: file code -> (name in English, name in its own language).
LANGUAGES = OrderedDict([
    ("fr", ("French", "Français")),
    ("de", ("German", "Deutsch")),
    ("it", ("Italian", "Italiano")),
    ("es", ("Spanish", "Español")),
    ("nl", ("Dutch", "Nederlands")),
    ("da", ("Danish", "Dansk")),
    ("fi", ("Finnish", "Suomi")),
    ("nb", ("Norwegian", "Norsk")),
    ("sv", ("Swedish", "Svenska")),
    ("ru", ("Russian", "Русский")),
    ("pt_BR", ("Brazilian Portuguese", "Português (Brasil)")),
])

# --------------------------------------------------------------------------
# The game's font (Titans Small / Large, the disc's package b4c85fe7): the
# characters it can draw. Read from its glyph table (findings/49 s.7). A text
# with any other character shows a gap there in the game.
FONT = set(chr(c) for c in range(0x20, 0x7F))              # ASCII
FONT |= set(chr(c) for c in range(0xA0, 0x100)) - {"×", "÷"}  # Latin-1...
# ...but U+00A5-00BE are the BUTTON PICTURES of this font (findings/23 s.1.2: ± = A,
# ² = B, µ = Back, » = an arrow...), all but © ® °: a translation must not use them
# as letters (« » quotes would draw as buttons).
BUTTON_PICTURES = set(chr(c) for c in range(0xA5, 0xBF)) - {"©", "®", "°"}
FONT -= BUTTON_PICTURES
FONT |= set("ĆćČčŁłŃńŘřŚśŠšŸŽž")                           # some Central European
FONT |= set(chr(c) for c in range(0x0410, 0x0450))         # Cyrillic А-я (no Ё ё)
FONT |= set("’†…")
FONT |= set(chr(c) for c in range(0xE000, 0xE200))         # the port's button pictures
FONT |= {"\n"}
# What translators reach for that the font lacks, and what to use instead.
FONT_HINTS = {
    "“": '"', "”": '"', "„": '"', "«": '"', "»": '"', "‘": "'",
    "–": "-", "—": "-", "€": "EUR", "•": "-", "Ё": "Е", "ё": "е", "«": '"', "»": '"',
}

# --------------------------------------------------------------------------
# Extracting texts from the code.

CALL = re.compile(r'\b(trcn|trc|trn|tr)\(')
STRING = re.compile(r'u?"((?:[^"\\\n]|\\.)*)"')
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'", "0": "\0"}


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            e = s[i + 1]
            if e == "u" and i + 5 < len(s) + 1:
                out.append(chr(int(s[i + 2:i + 6], 16)))
                i += 6
                continue
            out.append(ESCAPES.get(e, e))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def read_literals(text, pos, wide):
    """Reads one or more adjacent string literals from `pos` (skipping
    whitespace and comments between them). Returns (value, end) or None."""
    parts = []
    while True:
        m = re.compile(r'(?:\s|//[^\n]*\n|/\*.*?\*/)*', re.S).match(text, pos)
        pos = m.end()
        prefix = 'u"' if wide else '"'
        if not text.startswith(prefix, pos):
            break
        m = STRING.match(text, pos)
        if not m:
            break
        parts.append(unescape(m.group(1)))
        pos = m.end()
    if not parts:
        return None
    return "".join(parts), pos


def extract():
    """Every tr/trc/trn text in src/: OrderedDict (context, english) ->
    {"refs": [file:line], "notes": [...]}, in file order."""
    found = OrderedDict()
    skipped = []
    for folder, dirs, files in os.walk(SRC):
        dirs.sort()
        for name in sorted(files):
            if not name.endswith((".cpp", ".h", ".inc")):
                continue
            path = os.path.join(folder, name)
            rel = os.path.relpath(path, ROOT)
            if rel in (os.path.join("src", "language", "texts.h"), os.path.join("src", "language", "texts.cpp")):
                continue  # the lookup itself: its examples are documentation
            text = open(path, encoding="utf-8").read()
            lines = text.split("\n")
            for m in CALL.finditer(text):
                # Not a call: part of another name ("str(", "Patr(") is ruled
                # out by \b; a declaration/definition of tr itself is skipped.
                kind = m.group(1)
                pos = m.end()
                context = ""
                if kind in ("trc", "trcn"):
                    got = read_literals(text, pos, wide=False)
                    if not got:
                        skipped.append(f"{rel}:{text.count(chr(10), 0, m.start()) + 1}")
                        continue
                    context, pos = got
                    comma = re.compile(r'\s*,').match(text, pos)
                    if not comma:
                        continue
                    pos = comma.end()
                got = read_literals(text, pos, wide=True)
                if not got:
                    # tr(variable): a text marked elsewhere (trn) -> fine.
                    continue
                english, _ = got
                line_no = text.count("\n", 0, m.start()) + 1
                # "// TR: note" lines right above the call's line.
                notes = []
                k = line_no - 2
                while k >= 0 and lines[k].strip().startswith("// TR:"):
                    notes.insert(0, lines[k].strip()[len("// TR:"):].strip())
                    k -= 1
                entry = found.setdefault((context, english), {"refs": [], "notes": []})
                entry["refs"].append(f"{rel}:{line_no}")
                for n in notes:
                    if n not in entry["notes"]:
                        entry["notes"].append(n)
    if skipped:
        print("trc() without a plain context string (not extracted):", ", ".join(skipped), file=sys.stderr)
    return found


# --------------------------------------------------------------------------
# Reading and writing .po files.

class Entry:
    def __init__(self):
        self.context = ""
        self.english = ""
        self.translation = ""
        self.fuzzy = False
        self.comments = []     # "# ..." translator's own comments (kept)
        self.notes = []        # "#. ..." from the code
        self.refs = []         # "#: file:line"
        self.obsolete = False

    @property
    def key(self):
        return (self.context, self.english)


def read_po(path):
    """Returns (header text, [Entry])."""
    header, entries = "", []
    cur, field, obsolete = Entry(), None, False

    def finish():
        nonlocal cur, field, header
        if cur.english or cur.translation or cur.context:
            if not cur.english and not cur.context:
                header = cur.translation
            else:
                entries.append(cur)
        cur, field = Entry(), None

    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n").rstrip("\r")
        if not line.strip():
            finish()
            continue
        if line.startswith("#~"):
            cur.obsolete = True
            line = line[2:].lstrip()
            if not line:
                continue
        elif line.startswith("#,"):
            cur.fuzzy = "fuzzy" in line
            continue
        elif line.startswith("#."):
            cur.notes.append(line[2:].strip())
            continue
        elif line.startswith("#:"):
            cur.refs.extend(line[2:].split())
            continue
        elif line.startswith("#"):
            cur.comments.append(line[1:].strip())
            continue
        m = re.match(r'(msgctxt|msgid|msgstr)\s+"(.*)"\s*$', line)
        if m:
            field = {"msgctxt": "context", "msgid": "english", "msgstr": "translation"}[m.group(1)]
            setattr(cur, field, getattr(cur, field) + unescape(m.group(2)))
            continue
        m = re.match(r'"(.*)"\s*$', line)
        if m and field:
            setattr(cur, field, getattr(cur, field) + unescape(m.group(1)))
    finish()
    return header, entries


def quote(s):
    s = s.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    if "\n" in s.rstrip("\n"):
        # Multi-line texts: one quoted line per line (gettext's own style).
        parts = s.replace("\n", "\\n\n").split("\n")
        return '""\n' + "\n".join(f'"{p}"' for p in parts if p)
    return '"' + s.replace("\n", "\\n") + '"'


def write_po(path, header, entries):
    out = []
    out.append('msgid ""')
    out.append("msgstr " + quote(header))
    for e in entries:
        out.append("")
        p = "#~ " if e.obsolete else ""
        for c in e.comments:
            out.append(f"# {c}".rstrip())
        if not e.obsolete:
            for n in e.notes:
                out.append(f"#. {n}")
            if e.refs:
                out.append("#: " + " ".join(e.refs))
        if e.fuzzy:
            out.append("#, fuzzy")
        for keyword, value in (("msgctxt", e.context), ("msgid", e.english), ("msgstr", e.translation)):
            if keyword == "msgctxt" and not e.context:
                continue
            for line in (keyword + " " + quote(value)).split("\n"):
                out.append(p + line)  # obsolete entries: "#~ " on every line
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def header_for(code=None):
    lines = [
        "Project-Id-Version: Mind over Recomp",
        "Report-Msgid-Bugs-To: https://github.com/Maxilure/MindOver-Recomp/issues",
        "MIME-Version: 1.0",
        "Content-Type: text/plain; charset=UTF-8",
        "Content-Transfer-Encoding: 8bit",
    ]
    if code:
        lines.insert(1, f"Language: {code}")
    return "\n".join(lines) + "\n"


def template_entries(found):
    entries = []
    for (context, english), info in found.items():
        e = Entry()
        e.context, e.english = context, english
        e.notes = list(info["notes"])
        e.refs = list(info["refs"])
        entries.append(e)
    return entries


def cmd_extract():
    found = extract()
    os.makedirs(LANG_DIR, exist_ok=True)
    write_po(TEMPLATE, header_for(), template_entries(found))
    print(f"template.pot: {len(found)} texts")
    return found


def merge(code, found):
    path = os.path.join(LANG_DIR, code + ".po")
    if os.path.exists(path):
        header, old = read_po(path)
    else:
        header, old = header_for(code), []
    by_key = {e.key: e for e in old}
    merged = []
    for e in template_entries(found):
        prev = by_key.pop(e.key, None)
        if prev:
            e.translation, e.fuzzy, e.comments = prev.translation, prev.fuzzy, prev.comments
        merged.append(e)
    for e in by_key.values():  # texts the code doesn't have any more
        if e.translation:
            e.obsolete = True
            merged.append(e)
    write_po(path, header, merged)
    done = sum(1 for e in merged if not e.obsolete and e.translation)
    print(f"{code}.po: {done}/{len(found)} translated")


def cmd_update():
    found = cmd_extract()
    for code in LANGUAGES:
        if os.path.exists(os.path.join(LANG_DIR, code + ".po")):
            merge(code, found)


def cmd_new(code):
    found = extract()
    merge(code, found)


def placeholders(s):
    return sorted(re.findall(r"\{\d\}", s))


def cmd_check(codes=None):
    problems = 0
    for code in codes or LANGUAGES:
        path = os.path.join(LANG_DIR, code + ".po")
        if not os.path.exists(path):
            continue
        _, entries = read_po(path)
        live = [e for e in entries if not e.obsolete]
        missing = [e for e in live if not e.translation]
        for e in live:
            if not e.translation:
                continue
            where = f"{code}.po: {e.english[:50]!r}"
            if placeholders(e.english) != placeholders(e.translation):
                print(f"{where}: placeholders {placeholders(e.english)} vs {placeholders(e.translation)}")
                problems += 1
            bad = sorted(set(c for c in e.translation if c not in FONT and c not in e.english))
            if bad:
                hint = ", ".join(f"{c!r} -> {FONT_HINTS[c]!r}" for c in bad if c in FONT_HINTS)
                print(f"{where}: the game's font has no {' '.join(bad)}" + (f" (use {hint})" if hint else ""))
                problems += 1
            if e.english.count("\n") != e.translation.count("\n"):
                print(f"{where}: {e.english.count(chr(10)) + 1} lines in English, "
                      f"{e.translation.count(chr(10)) + 1} here")
                problems += 1
        if missing:
            print(f"{code}.po: {len(missing)} texts not translated yet")
    print("no problems" if not problems else f"{problems} problems")
    return problems


def cmd_status():
    rows = []
    for code, (english_name, own) in LANGUAGES.items():
        path = os.path.join(LANG_DIR, code + ".po")
        if not os.path.exists(path):
            continue
        _, entries = read_po(path)
        live = [e for e in entries if not e.obsolete]
        total = len(live) or 1
        translated = sum(1 for e in live if e.translation)
        checked = sum(1 for e in live if e.translation and not e.fuzzy)
        rows.append(f"| {english_name} ({own}) | [`{code}.po`]({code}.po) | {translated * 100 // total}% "
                    f"| {checked * 100 // total}% |")
    table = ("| Language | File | Translated | Checked by a speaker |\n|---|---|---|---|\n"
             + "\n".join(rows))
    text = open(README, encoding="utf-8").read()
    new = re.sub(r"(<!-- status -->\n).*?(\n<!-- /status -->)", lambda m: m.group(1) + table + m.group(2),
                 text, flags=re.S)
    with open(README, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
    print(table)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    if cmd == "extract":
        cmd_extract()
    elif cmd == "update":
        cmd_update()
    elif cmd == "check":
        return 1 if cmd_check(sys.argv[2:] or None) else 0
    elif cmd == "status":
        cmd_status()
    elif cmd == "new" and len(sys.argv) == 3:
        cmd_new(sys.argv[2])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
