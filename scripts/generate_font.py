"""Generate reproducible LVGL fonts from Noto Sans CJK SC (OFL).

16 px covers all of GB2312 (6763 hanzi plus its symbols and kana), so network text such as hitokoto sentences
renders; 14 px is a subset of the characters used in the firmware source.
"""
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
characters = set()
for source in (root / "firmware/main").glob("*.c"):
    if not source.name.startswith("display_font"):
        characters.update(c for c in source.read_text() if "\u4e00" <= c <= "\u9fff" or c == "\u00b7")
gb2312 = set()
for high in range(0xA1, 0xF8):
    for low in range(0xA1, 0xFF):
        try:
            gb2312.add(bytes([high, low]).decode("gb2312"))
        except UnicodeDecodeError:
            pass
# Python maps the GB2312 dash and middle dot to U+2015 and U+30FB; text on the web uses these instead.
gb2312.update("\u2014\u2013\u00b7")
for size, name, symbols in ((16, "display_font", gb2312 | characters), (14, "display_font_14", characters)):
    subprocess.run([str(root / ".tools/font/node_modules/.bin/lv_font_conv"),
        "--font", str(root / ".tools/NotoSansCJKsc-Regular.otf"), "--range", "0x20-0x7E",
        "--symbols", "".join(sorted(symbols)), "--size", str(size), "--bpp", "4", "--no-compress",
        "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", name, "--output",
        str(root / "firmware/main" / (name + ".c"))], check=True)
    print(f"Generated {name} ({size} px):", len(symbols), "characters plus ASCII")
