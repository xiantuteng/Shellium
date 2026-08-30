#!/usr/bin/env python3
"""Fill i18n/shellium_<lang>.ts from scripts/i18n/drafts/<lang>.tsv.

sources.json holds the ordered unique English source strings. Each drafts/<lang>.tsv
line is "<index>\\t<translation>" (index into sources.json). Translations are
written as <translation type="unfinished"> so Qt Linguist flags every one for
review (lrelease still compiles unfinished entries with text into the .qm, so the
UI shows the draft). Format-string safety: if a translation's %1..%9 set differs
from the source, the English source is kept instead.

Run:  python3 scripts/i18n/fill_drafts.py [lang ...]
"""
import json
import re
import sys
import pathlib
import xml.etree.ElementTree as ET

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
I18N = ROOT / "i18n"
DRAFTS = HERE / "drafts"
SOURCES = json.load(open(HERE / "sources.json", encoding="utf-8"))

PCT = re.compile(r"%[1-9]")
ALL_LANGS = ["fr", "de", "it", "ja", "ko", "pt_BR", "ro", "ru", "es", "uk", "ar"]


def source_text(msg):
    src = msg.find("source")
    parts = [src.text or ""]
    for child in src:
        parts.append("\r" if child.get("value") == "xd" else "")
        parts.append(child.tail or "")
    return "".join(parts)


def load_tsv(lang):
    path = DRAFTS / ("%s.tsv" % lang)
    if not path.exists():
        return {}
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "\t" not in line:
            continue
        idx, tr = line.split("\t", 1)
        try:
            src = SOURCES[int(idx)]
        except (ValueError, IndexError):
            continue
        tr = tr.replace("\\n", "\n").replace("\\r", "\r").replace("\\t", "\t").strip()
        if tr:
            out[src] = tr
    return out


def fill(lang):
    mapping = load_tsv(lang)
    if not mapping:
        print("skip %s (no drafts/%s.tsv)" % (lang, lang))
        return
    tspath = I18N / ("shellium_%s.ts" % lang)
    tree = ET.parse(tspath)
    filled = kept = 0
    for ctx in tree.getroot().findall("context"):
        for msg in ctx.findall("message"):
            src = source_text(msg)
            cand = mapping.get(src)
            if not cand:
                continue
            if sorted(PCT.findall(cand)) != sorted(PCT.findall(src)):
                kept += 1
                continue
            tr = msg.find("translation")
            tr.text = cand
            tr.set("type", "unfinished")
            for c in list(tr):
                tr.remove(c)
            filled += 1
    tree.write(tspath, encoding="utf-8", xml_declaration=True)
    txt = tspath.read_text().replace(
        "<?xml version='1.0' encoding='utf-8'?>",
        '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>',
    )
    tspath.write_text(txt)
    print("%s: %d filled, %d kept English (%% mismatch), %d in tsv"
          % (tspath.name, filled, kept, len(mapping)))


if __name__ == "__main__":
    for lang in (sys.argv[1:] or ALL_LANGS):
        fill(lang)
