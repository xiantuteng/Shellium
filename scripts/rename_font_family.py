#!/usr/bin/env python3
"""把 TTF 的字体族名改写成一个独立族名，供 Qt 当作单独的一项列出。

上游的 Medium 字重文件在 name 表里仍属于基础族（例如 FiraMono-Medium.ttf 的 name ID 1 是
"Fira Mono"），Qt 会把它并进基础族，设置界面因此选不到中等字重。本脚本改写 name ID 1/2/4/6，
并删除排版族名 ID 16/17——只改 ID 1 而留着 ID 16，Qt 仍会按 ID 16 把字体并回原族。原厂
DSIG 在改名后必然失效，一并删除。

依赖 fonttools（pip install fonttools）。resources/fonts 下两个 Medium 文件的生成命令：

    python3 scripts/rename_font_family.py SourceCodePro-Medium.ttf \
        resources/fonts/SourceCodeProMedium-Regular.ttf \
        "Source Code Pro Medium" SourceCodeProMedium-Regular
    python3 scripts/rename_font_family.py FiraMono-Medium.ttf \
        resources/fonts/FiraMonoMedium-Regular.ttf \
        "Fira Mono Medium" FiraMonoMedium-Regular
"""

import sys

from fontTools.ttLib import TTFont


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2

    source, destination, family, postscript_name = sys.argv[1:5]
    font = TTFont(source)
    name_table = font["name"]

    # 先删排版族名，否则下面改出来的族名会被 Qt 按 ID 16 重新归并回原始族。
    name_table.names = [n for n in name_table.names if n.nameID not in (16, 17)]

    # Windows(3,1,0x409) 与 Macintosh(1,0,0) 两套记录都要写：不同平台的字体后端读的记录不同，
    # 只改一套会出现“Qt 里改名了、系统字体册里没改”的错位。
    for platform in ((3, 1, 0x409), (1, 0, 0)):
        platform_id, encoding_id, language_id = platform
        name_table.setName(family, 1, platform_id, encoding_id, language_id)
        name_table.setName("Regular", 2, platform_id, encoding_id, language_id)
        name_table.setName(family, 4, platform_id, encoding_id, language_id)
        name_table.setName(postscript_name, 6, platform_id, encoding_id, language_id)

    if "DSIG" in font:
        del font["DSIG"]

    font.save(destination)
    print(f"{destination} -> {family}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
