#!/usr/bin/env python3
"""Machine-translation FIRST DRAFTS for the untranslated languages.

Reads the English sources from i18n/shellium_zh_CN.ts, runs them through
translate-shell (Google Translate free endpoint) per target language, and writes
i18n/shellium_<lang>.ts with drafts marked <translation type="unfinished"> so Qt
Linguist flags every entry for human review.

Safeguards:
  * %1..%9 placeholders are shielded as U+E001..U+E009 during MT and restored
    afterwards; if the restored set != the source set, the English source is kept
    (never ship a format string with the wrong argument count).
  * Embedded newlines are shielded (the batch delimiter is a real newline).
  * Batches of 20; on any line-count drift or [ERROR] the batch is retried
    string-by-string, and a still-failing string keeps its English.
  * ~1.2 s pause between requests; 3 retries with backoff.

Run:  python3 scripts/i18n/mt_drafts.py [lang ...]
"""
import subprocess
import sys
import time
import re
import pathlib
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[2]
I18N = ROOT / "i18n"

LANGS = {
    "fr": "fr", "de": "de", "it": "it", "ja": "ja", "ko": "ko",
    "pt_BR": "pt-BR", "ro": "ro", "ru": "ru", "es": "es", "uk": "uk", "ar": "ar",
}

PH_BASE = 0xE000
BR = chr(0xE00E)
PH_RE = re.compile("[" + chr(PH_BASE + 1) + "-" + chr(PH_BASE + 9) + "]")
PCT_RE = re.compile(r"%([1-9])")


def source_text(msg):
    src = msg.find("source")
    parts = [src.text or ""]
    for child in src:
        parts.append("\r" if child.get("value") == "xd" else "")
        parts.append(child.tail or "")
    return "".join(parts)


def shield(s):
    s = s.replace("\r\n", "\n")
    s = PCT_RE.sub(lambda m: chr(PH_BASE + int(m.group(1))), s)
    return s.replace("\n", BR)


def unshield(s):
    s = s.replace(BR, "\n")
    return PH_RE.sub(lambda m: "%" + str(ord(m.group(0)) - PH_BASE), s)


def placeholders(s):
    return sorted(re.findall(r"%[1-9]", s))


def trans_lines(lines, target):
    payload = "\n".join(lines) + "\n"
    for attempt in range(3):
        p = subprocess.run(
            ["trans", "-b", "-no-autocorrect", "-no-warn", "-s", "en", "-t", target],
            input=payload, capture_output=True, text=True,
        )
        out = p.stdout.rstrip("\n").split("\n")
        if all("[ERROR]" not in ln for ln in out) and len(out) == len(lines):
            return out
        time.sleep(1.5 * (attempt + 1))
    raise RuntimeError("drift")


def translate_all(sources, target, checkpoint=None):
    result = {}
    batch = 40
    for i in range(0, len(sources), batch):
        chunk = sources[i:i + batch]
        try:
            pairs = list(zip(chunk, trans_lines([shield(s) for s in chunk], target)))
        except RuntimeError:
            pairs = []
            for src in chunk:
                try:
                    pairs.append((src, trans_lines([shield(src)], target)[0]))
                except RuntimeError:
                    pairs.append((src, shield(src)))
                time.sleep(1.2)
        for src, raw in pairs:
            cand = unshield(raw).strip()
            if not cand or placeholders(cand) != placeholders(src):
                cand = src
            result[src] = cand
        print("  %s: %d/%d" % (target, min(i + batch, len(sources)), len(sources)), flush=True)
        if checkpoint and (i // batch) % 8 == 7:
            checkpoint(dict(result))
        time.sleep(0.4)
    return result


def already_translated(lang):
    """Sources that shellium_<lang>.ts already has a non-empty translation for."""
    path = I18N / ("shellium_%s.ts" % lang)
    done = set()
    for ctx in ET.parse(path).getroot().findall("context"):
        for msg in ctx.findall("message"):
            tr = msg.find("translation")
            if tr is not None and (tr.text or "").strip():
                done.add(source_text(msg))
    return done


def write_ts(lang, mapping):
    path = I18N / ("shellium_%s.ts" % lang)
    tree = ET.parse(path)
    filled = 0
    for ctx in tree.getroot().findall("context"):
        for msg in ctx.findall("message"):
            src = source_text(msg)
            if mapping.get(src) and mapping[src] != src:
                tr = msg.find("translation")
                tr.text = mapping[src]
                tr.set("type", "unfinished")
                for c in list(tr):
                    tr.remove(c)
                filled += 1
    tree.write(path, encoding="utf-8", xml_declaration=True)
    txt = path.read_text().replace(
        "<?xml version='1.0' encoding='utf-8'?>",
        '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>',
    )
    path.write_text(txt)
    print("%s: %d/%d draft translations written" % (path.name, filled, len(mapping)))


def main():
    root = ET.parse(I18N / "shellium_zh_CN.ts").getroot()
    seen, sources = set(), []
    for ctx in root.findall("context"):
        for msg in ctx.findall("message"):
            s = source_text(msg)
            if s and s not in seen:
                seen.add(s)
                sources.append(s)
    for lang in (sys.argv[1:] or list(LANGS)):
        done = already_translated(lang)
        todo = [s for s in sources if s not in done]
        print("=== %s: %d done, %d to translate ===" % (lang, len(done), len(todo)), flush=True)
        if not todo:
            continue

        def load_existing():
            ex = {}
            for c in ET.parse(I18N / ("shellium_%s.ts" % lang)).getroot().findall("context"):
                for mm in c.findall("message"):
                    t = mm.find("translation")
                    if t is not None and (t.text or "").strip():
                        ex[source_text(mm)] = t.text
            return ex

        def checkpoint(partial):
            merged = load_existing()
            merged.update(partial)
            write_ts(lang, merged)

        mapping = translate_all(todo, LANGS[lang], checkpoint)
        checkpoint(mapping)


if __name__ == "__main__":
    main()
