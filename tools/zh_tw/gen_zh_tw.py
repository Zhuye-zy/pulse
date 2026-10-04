#!/usr/bin/env python3
"""Generate Traditional Chinese (Taiwan) resources from the Simplified tables.

    pip install opencc-python-reimplemented
    python tools/zh_tw/gen_zh_tw.py          # rewrite generated files
    python tools/zh_tw/gen_zh_tw.py --check  # fail if they are stale

Inputs:  src/common/strings.rcinc (LANGUAGE 0x04, 0x02 blocks)
         tools/zh_tw/glossary.tsv   Pulse / Windows zh-TW terms, applied before OpenCC s2twp
         tools/zh_tw/overrides.tsv  exact per-id text for ambiguous strings
         tools/zh_tw/vocabulary.txt common words added to the runtime phrase table
         src/**/*.cpp|h             Simplified UI literals written in code
Output:  src/common/strings_zh_tw.rcinc (LANGUAGE 0x04, 0x01)
         src/common/zh_hant_table.inc  phrase and character tables for l10n::Cn / Pick / HantText
"""
import re
import sys
from pathlib import Path

from opencc import OpenCC

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SOURCE = ROOT / "src/common/strings.rcinc"
OUTPUT = ROOT / "src/common/strings_zh_tw.rcinc"
TABLE = ROOT / "src/common/zh_hant_table.inc"

# Literals in these files are data (shell menu names, search syntax, collation
# boundaries) or test fixtures, never shown through l10n::Cn / Pick / HantText.
EXCLUDED = (
    "src/bench/",
    "src/ipc/ctx_menu_util.h",
    "src/index/search_kinds.cpp",
    "src/index/index_query.cpp",
    "src/app/entry_group.cpp",
    "src/app/selftest_1b2.cpp",
)

_cc = OpenCC("s2twp")
_cc_chars = OpenCC("s2t")


def load_tsv(path):
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        key, value = line.split("\t", 1)
        rows.append((key, value))
    return rows


GLOSSARY = sorted(load_tsv(HERE / "glossary.tsv"), key=lambda kv: -len(kv[0]))


QUOTES = str.maketrans({"\u201c": "\u300c", "\u201d": "\u300d", "\u2018": "\u300e", "\u2019": "\u300f"})


def to_hant(text):
    """Simplified -> Traditional (Taiwan), with the Pulse glossary protected."""
    protected = []
    for simplified, traditional in GLOSSARY:
        if simplified in text:
            marker = chr(0xE000 + len(protected))
            protected.append(traditional)
            text = text.replace(simplified, marker)
    text = _cc.convert(text)
    for i, traditional in enumerate(protected):
        text = text.replace(chr(0xE000 + i), traditional)
    # 台灣慣用直角引號。
    return text.translate(QUOTES)


HAN = re.compile(r"[\u3400-\u9fff]")
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|(L?)"((?:[^"\\\n]|\\.)*)"|\'(?:[^\'\\\n]|\\.)*\'', re.S)
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'", "?": "?"}


def decode(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n in "uU":
            width = 4 if n == "u" else 8
            out.append(chr(int(body[i + 2:i + 2 + width], 16)))
            i += 2 + width
        elif n == "x":
            j = i + 2
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(chr(int(body[i + 2:j], 16)))
            i = j
        else:
            out.append(ESCAPES.get(n, n))
            i += 2
    return "".join(out)


def source_literals():
    """Wide string literals with Han characters; adjacent literals are joined."""
    found = set()
    for path in sorted((ROOT / "src").rglob("*")):
        rel = path.relative_to(ROOT).as_posix()
        if path.suffix not in (".cpp", ".h") or rel.startswith(EXCLUDED):
            continue
        if path.name.endswith(("_test.cpp", "_test.h")):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        pending, pending_end = None, 0

        def flush():
            if pending is not None and HAN.search(pending):
                found.add(pending)

        for m in TOKEN.finditer(text):
            if m.group(2) is not None and m.group(1) == "L":
                value = decode(m.group(2))
                if pending is not None and not text[pending_end:m.start()].strip():
                    pending += value
                else:
                    flush()
                    pending = value
                pending_end = m.end()
            else:
                flush()
                pending = None
        flush()
    return found


def cpp_literal(text):
    out = []
    for c in text:
        o = ord(c)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        elif 0x20 <= o < 0x7F:
            out.append(c)
        else:
            if o > 0xFFFF:
                sys.exit(f"non-BMP character in UI literal: {text!r}")
            out.append(f"\\u{o:04x}")
    return 'L"' + "".join(out) + '"'


def vocabulary():
    words = {k for k, _ in GLOSSARY}
    path = HERE / "vocabulary.txt"
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            words.add(line)
    return words


def generate_table():
    phrases = {}
    for key in source_literals() | vocabulary():
        value = to_hant(key)
        if value != key:
            phrases[key] = value
    # Every CJK unified ideograph whose standard Traditional form differs; used
    # for text outside the phrase table.
    chars = {}
    for cp in range(0x4E00, 0xA000):
        c = chr(cp)
        t = _cc_chars.convert(c)
        # wchar_t pairs: skip the few rare targets outside the BMP.
        if len(t) == 1 and t != c and ord(t) <= 0xFFFF:
            chars[c] = t
    out = ["// Generated by tools/zh_tw/gen_zh_tw.py from Simplified UI literals in src/.",
           "// Do not edit by hand. Sorted by UTF-16 code unit for binary search.", "",
           "constexpr ZhHantPhrase kZhHantPhrases[] = {"]
    for key in sorted(phrases):
        out.append(f"    {{{cpp_literal(key)}, {cpp_literal(phrases[key])}}},")
    out += ["};", "", "constexpr ZhHantChar kZhHantChars[] = {"]
    for key in sorted(chars):
        out.append(f"    {{0x{ord(key):04X}, 0x{ord(chars[key]):04X}}},")
    out.append("};")
    return "\n".join(out) + "\n"


def generate():
    overrides = dict(load_tsv(HERE / "overrides.tsv"))
    out = ["// Generated by tools/zh_tw/gen_zh_tw.py from strings.rcinc. Do not edit by hand:",
           "// change tools/zh_tw/glossary.tsv or overrides.tsv and regenerate.", ""]
    in_zh = False
    used = set()
    for line in SOURCE.read_text(encoding="utf-8").splitlines():
        if line.startswith("LANGUAGE"):
            in_zh = "0x04, 0x02" in line
            if in_zh:
                out.append("LANGUAGE 0x04, 0x01")
            continue
        if not in_zh or line.startswith("#include"):
            continue
        match = re.match(r'(\s*)(IDS_\w+)(\s+)"(.*)"(\s*)$', line)
        if match:
            indent, key, gap, text, tail = match.groups()
            if key in overrides:
                text = overrides[key]
                used.add(key)
            else:
                text = to_hant(text)
            out.append(f'{indent}{key}{gap}"{text}"{tail}')
        else:
            out.append(line)
            if line.strip() == "END":
                out.append("")
                in_zh = False
    unused = set(overrides) - used
    if unused:
        sys.exit(f"overrides.tsv: unknown ids {sorted(unused)}")
    return "\n".join(out).rstrip() + "\n"


def main():
    outputs = {OUTPUT: generate(), TABLE: generate_table()}
    if "--check" in sys.argv:
        stale = [p for p, text in outputs.items()
                 if not p.exists() or p.read_text(encoding="utf-8") != text]
        if stale:
            names = ", ".join(str(p.relative_to(ROOT)) for p in stale)
            sys.exit(f"{names} stale; run tools/zh_tw/gen_zh_tw.py")
        print("zh-TW resources are up to date")
        return
    for path, text in outputs.items():
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {path.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
