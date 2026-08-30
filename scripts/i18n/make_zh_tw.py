#!/usr/bin/env python3
"""Generate i18n/shellium_zh_TW.ts from shellium_zh_CN.ts via OpenCC (s2twp:
Simplified -> Traditional with Taiwan-standard phrasing)."""
import subprocess
import xml.etree.ElementTree as ET
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "i18n" / "shellium_zh_CN.ts"
DST = ROOT / "i18n" / "shellium_zh_TW.ts"

tree = ET.parse(SRC)
root = tree.getroot()
root.set("language", "zh_TW")

nodes = []
for ctx in root.findall("context"):
    for msg in ctx.findall("message"):
        tr = msg.find("translation")
        if tr is not None and tr.text:
            nodes.append(tr)

SEP = "\n␟\n"  # unit-separator, never appears in UI strings
blob = SEP.join(n.text for n in nodes)
converted = subprocess.run(
    ["opencc", "-c", "s2twp"], input=blob, capture_output=True, text=True, check=True
).stdout
parts = converted.split(SEP)
assert len(parts) == len(nodes), (len(parts), len(nodes))

for node, text in zip(nodes, parts):
    node.text = text
    node.attrib.pop("type", None)

tree.write(DST, encoding="utf-8", xml_declaration=True)
text = DST.read_text().replace(
    "<?xml version='1.0' encoding='utf-8'?>",
    '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>',
)
DST.write_text(text)
print(f"wrote {DST.name}: {len(nodes)} entries converted (s2twp)")
