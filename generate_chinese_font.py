"""把固定的界面文字生成 16x16 点阵字库头文件。

用途：只给**固定 UI 文案**用（播放页 / 歌曲列表 / 设置 这些不会变的字）。
歌名是任意字，走 SD 卡上的完整字库，不走这里。

用法：
    python generate_chinese_font.py                     # 自动找常见中文字体
    python generate_chinese_font.py D:/fonts/xxx.ttf    # 指定字体文件
    FONT_PATH=/path/to/font.ttf python generate_chinese_font.py

依赖：Pillow（pip install pillow）
"""

import os
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUTPUT_PATH = Path("chinese_font.h")

# 依次尝试：命令行参数 > 环境变量 > 常见中文字体路径
FONT_CANDIDATES = [
    "C:/Windows/Fonts/simhei.ttf",       # Windows 黑体（16px 下最清晰）
    "C:/Windows/Fonts/msyh.ttc",         # Windows 微软雅黑
    "/System/Library/Fonts/PingFang.ttc",  # macOS
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",  # Linux
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",            # Linux 文泉驿
]

# 界面里会出现的所有字，改 UI 文案时记得同步加到这里
CHARACTERS = (
    "播放停止暂上一首下曲目就绪中音量卡片无文件返回来前后设置完成打开失败"
    "已列表歌曲长按选择页封面共●…↑↓连调加减快"
)

GLYPH_SIZE = 16


def resolve_font_path():
    if len(sys.argv) > 1:
        return Path(sys.argv[1])

    env_path = os.environ.get("FONT_PATH")
    if env_path:
        return Path(env_path)

    for candidate in FONT_CANDIDATES:
        if Path(candidate).exists():
            return Path(candidate)

    raise SystemExit(
        "找不到中文字体。请显式指定：\n"
        "    python generate_chinese_font.py C:/path/to/font.ttf"
    )


def build_glyph(font, character):
    """把一个字符渲染成 16 行 x 16 列的点阵，居中放置。"""
    image = Image.new("1", (GLYPH_SIZE, GLYPH_SIZE), 0)
    draw = ImageDraw.Draw(image)
    left, top, right, bottom = draw.textbbox((0, 0), character, font=font)
    width = right - left
    height = bottom - top
    x = (GLYPH_SIZE - width) // 2 - left
    y = (GLYPH_SIZE - height) // 2 - top
    draw.text((x, y), character, font=font, fill=1)

    rows = []
    for row in range(GLYPH_SIZE):
        bits = 0
        for column in range(GLYPH_SIZE):
            if image.getpixel((column, row)):
                bits |= 1 << (GLYPH_SIZE - 1 - column)
        rows.append(bits)
    return rows


def main():
    font_path = resolve_font_path()
    font = ImageFont.truetype(str(font_path), GLYPH_SIZE)

    glyphs = []
    missing = []
    for character in CHARACTERS:
        rows = build_glyph(font, character)
        if not any(rows):
            missing.append(character)
        glyphs.append((ord(character), rows))

    lines = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "// 由 generate_chinese_font.py 自动生成，不要手改。",
        f"// 字源：{font_path.name}  {GLYPH_SIZE}x{GLYPH_SIZE} 点阵",
        f"// 共 {len(glyphs)} 个字形，约占 {len(glyphs) * (GLYPH_SIZE * 2 + 4)} 字节",
        "",
        "struct ChineseGlyph {",
        "  uint32_t codepoint;",
        f"  uint16_t rows[{GLYPH_SIZE}];",
        "};",
        "",
        "static const ChineseGlyph chineseGlyphs[] = {",
    ]

    for codepoint, rows in glyphs:
        row_text = ", ".join(f"0x{row:04X}" for row in rows)
        lines.append(f"  {{0x{codepoint:04X}, {{{row_text}}}}},")

    lines.extend(
        [
            "};",
            "",
            "static const size_t chineseGlyphCount =",
            "    sizeof(chineseGlyphs) / sizeof(chineseGlyphs[0]);",
            "",
        ]
    )

    OUTPUT_PATH.write_text("\n".join(lines), encoding="utf-8")
    print(f"Generated {OUTPUT_PATH} with {len(glyphs)} glyphs from {font_path}.")
    if missing:
        print(f"!! 这些字在该字体里是空白的，换字或换字体：{''.join(missing)}")
    else:
        print("All glyphs rendered OK.")


if __name__ == "__main__":
    main()
