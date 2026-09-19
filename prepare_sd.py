"""把 SD 卡准备好，让播放器能显示中文/日文歌名和封面。

做两件事：
  1. 生成 font16.bin —— 中/日文 16x16 点阵字库（约 680 KB）
  2. 逐首歌抠出封面，缩放后转成 RGB565，存成 <歌名>.cov

封面来源优先级：
  MP3 内嵌的 ID3 APIC 图片  →  同目录同名图片(.jpg/.png/.bmp/.webp)  →  跳过

用法：
    pip install pillow
    python prepare_sd.py --music D:/CloudMusic --out D:/sd_ready

    --music   音乐目录（默认当前目录）
    --out     输出目录（默认 ./sd_ready），跑完把这个目录里的东西拷到 SD 卡根目录
    --size    封面边长，必须和固件里的 COVER_W/COVER_H 一致（默认 140）
    --no-font    只做封面
    --no-cover   只做字库

── 为什么要在 PC 上预处理 ──────────────────────────────────────────────
固件里不装 JPEG 解码器，也不做图片缩放。PC 上做完，板子只干两件事：
一次顺序读 + 一次整块刷屏。这样既不占 flash，也不会因为解码把音频卡爆。
代价：每次往卡里加新歌，要重跑一次本脚本。
"""

import argparse
import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# ============================================================================
# 字库参数（必须和 Mp3.ino 里的 FontSegment / 头部解析保持一致）
# ============================================================================

FONT_MAGIC = b"MPF1"
GLYPH_W = 16
GLYPH_H = 16
BYTES_PER_GLYPH = GLYPH_W * GLYPH_H // 8  # 位图按 1bpp 打包，16x16 = 32 字节

FONT_FILE = "font16.bin"

# 汉字/标点用黑体，假名用 MS Gothic（日文字形更准）
HANZI_FONT_CANDIDATES = [
    "C:/Windows/Fonts/simhei.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/System/Library/Fonts/PingFang.ttc",
]
KANA_FONT_CANDIDATES = [
    "C:/Windows/Fonts/msgothic.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
]

# (起始码点, 结束码点, 用哪个字体)
# 这几个区段加起来约 21600 字，覆盖了中/日文歌名里 99.9% 会用到的字符
FONT_RANGES = [
    (0x00A0, 0x00FF, "hanzi"),  # 拉丁字母补充：° × ÷ 之类
    (0x2000, 0x206F, "hanzi"),  # 通用标点：— – ' ' " " …
    (0x3000, 0x30FF, "kana"),   # CJK 标点 + 平假名 + 片假名（々 「 」 ・ お ユ ウ）
    (0x4E00, 0x9FFF, "hanzi"),  # CJK 统一汉字（中日共用）
    (0xFF00, 0xFFEF, "hanzi"),  # 全角形式：（ ） ？ ！
]

# ============================================================================
# 封面参数
# ============================================================================

COVER_EXT = ".cov"
IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".webp", ".gif")


def log(msg=""):
    print(msg, flush=True)


# ============================================================================
# 字库生成
# ============================================================================


def pick_font(candidates, what):
    for path in candidates:
        if Path(path).exists():
            return path
    raise SystemExit(
        f"找不到{what}字体。请改 prepare_sd.py 里的 *_FONT_CANDIDATES 列表。"
    )


def build_glyph(font, character):
    """把一个字符渲染成 16x16 位图，居中放置，返回 32 字节（每行 2 字节，高字节 = 左边 8 像素）。"""
    image = Image.new("1", (GLYPH_W, GLYPH_H), 0)
    draw = ImageDraw.Draw(image)
    left, top, right, bottom = draw.textbbox((0, 0), character, font=font)
    width = right - left
    height = bottom - top
    x = (GLYPH_W - width) // 2 - left
    y = (GLYPH_H - height) // 2 - top
    draw.text((x, y), character, font=font, fill=1)
    # PIL 的 "1" 模式每 8 像素打包成 1 字节，高位在左 —— 正好就是我们要的位序
    return image.tobytes()


def generate_font(out_dir):
    hanzi_path = pick_font(HANZI_FONT_CANDIDATES, "汉字")
    kana_path = pick_font(KANA_FONT_CANDIDATES, "假名")
    log(f"  汉字字体: {hanzi_path}")
    log(f"  假名字体: {kana_path}")

    hanzi_font = ImageFont.truetype(hanzi_path, GLYPH_H)
    kana_font = ImageFont.truetype(kana_path, GLYPH_H)
    fonts = {"hanzi": hanzi_font, "kana": kana_font}

    total = sum(end - start + 1 for start, end, _ in FONT_RANGES)
    log(f"  共 {total} 个字形，预计 {total * BYTES_PER_GLYPH / 1024:.0f} KB")

    # 先把所有字形数据攒出来，再算偏移写头部
    segment_headers = []
    body = bytearray()
    blank_count = 0
    data_offset = 16 + len(FONT_RANGES) * 12  # 头部 16 字节 + 每段 12 字节

    for start, end, which in FONT_RANGES:
        font = fonts[which]
        offset = data_offset + len(body)
        for codepoint in range(start, end + 1):
            glyph = build_glyph(font, chr(codepoint))
            if not any(glyph):
                blank_count += 1
            body += glyph
        segment_headers.append((start, end, offset))
        log(f"    U+{start:04X}-U+{end:04X}  完成")

    out_path = out_dir / FONT_FILE
    with open(out_path, "wb") as f:
        f.write(FONT_MAGIC)
        f.write(struct.pack("<HHHHI", GLYPH_W, GLYPH_H, BYTES_PER_GLYPH,
                            len(FONT_RANGES), 0))
        for start, end, offset in segment_headers:
            f.write(struct.pack("<III", start, end, offset))
        f.write(body)

    size_kb = out_path.stat().st_size / 1024
    log(f"  ✔ {out_path}  ({size_kb:.0f} KB)")
    if blank_count:
        log(f"  ⚠ 有 {blank_count} 个字形在该字体里是空白的（属正常，冷门码位）")
    return out_path


# ============================================================================
# 封面提取
# ============================================================================


def syncsafe(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]


def extract_embedded_cover(path):
    """从 ID3v2.3 / v2.4 的 APIC 帧里抠出内嵌图片，返回 bytes 或 None。"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None

    if len(data) < 10 or data[:3] != b"ID3":
        return None

    version = data[3]
    flags = data[5]
    tag_size = syncsafe(data[6:10])
    pos = 10
    if flags & 0x40:  # 扩展头
        pos += struct.unpack(">I", data[10:14])[0]

    end = min(10 + tag_size, len(data))
    while pos + 10 <= end:
        frame_id = data[pos:pos + 4]
        if not frame_id.strip(b"\x00"):
            break
        if version >= 4:
            size = syncsafe(data[pos + 4:pos + 8])
        else:
            size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        if size <= 0 or pos + 10 + size > len(data):
            break

        body = data[pos + 10:pos + 10 + size]
        if frame_id == b"APIC" and len(body) > 4:
            # APIC = 编码(1) + MIME(以 0 结尾) + 图片类型(1) + 描述(以 0 结尾) + 图片数据
            rest = body[1:]
            zero = rest.find(b"\x00")
            if zero < 0:
                return None
            rest = rest[zero + 1:]
            if not rest:
                return None
            rest = rest[1:]  # 跳过图片类型
            # 描述：UTF-16 时是双字节结尾，这里做个粗略判断
            if body[0] in (1, 2):
                end_desc = 0
                while end_desc + 1 < len(rest):
                    if rest[end_desc] == 0 and rest[end_desc + 1] == 0:
                        end_desc += 2
                        break
                    end_desc += 2
                rest = rest[end_desc:]
            else:
                zero = rest.find(b"\x00")
                if zero >= 0:
                    rest = rest[zero + 1:]
            if rest:
                return rest

        pos += 10 + size

    return None


def find_sidecar_image(mp3_path):
    for ext in IMAGE_EXTS:
        candidate = mp3_path.with_suffix(ext)
        if candidate.exists():
            return candidate
    return None


def image_to_rgb565(image, size):
    """转成 RGB565 小端字节流。公式必须和 Adafruit_GFX::color565 一致。"""
    raw = image.convert("RGB").resize((size, size), Image.LANCZOS).tobytes()
    out = bytearray()
    for i in range(0, len(raw), 3):
        value = ((raw[i] & 0xF8) << 8) | ((raw[i + 1] & 0xFC) << 3) | (raw[i + 2] >> 3)
        out.append(value & 0xFF)
        out.append(value >> 8)
    return bytes(out)


def list_mp3_files(music_dir):
    """列出目录里的 mp3。

    注意不能用 glob("*.mp3") + glob("*.MP3") —— Windows 的文件系统大小写不敏感，
    两个 pattern 会命中同一批文件，导致每首歌被处理两遍。
    """
    found = {}
    for entry in music_dir.iterdir():
        if entry.is_file() and entry.suffix.lower() == ".mp3":
            found[entry.name.lower()] = entry
    return [found[key] for key in sorted(found)]


def process_covers(music_dir, out_dir, size):
    mp3_files = list_mp3_files(music_dir)
    if not mp3_files:
        log(f"  ⚠ {music_dir} 里没有 .mp3")
        return 0, 0

    made = 0
    skipped = []

    for mp3 in mp3_files:
        name = mp3.stem
        cover_path = out_dir / (name + COVER_EXT)

        raw = extract_embedded_cover(mp3)
        source = "内嵌"

        if raw is None:
            sidecar = find_sidecar_image(mp3)
            if sidecar is None:
                skipped.append(name)
                log(f"    - {name}   没有封面")
                continue
            raw = sidecar.read_bytes()
            source = sidecar.suffix

        try:
            image = Image.open(io.BytesIO(raw))
            image.load()
        except Exception as exc:
            skipped.append(name)
            log(f"    ! {name}   图片打不开（{exc}）")
            continue

        cover_path.write_bytes(image_to_rgb565(image, size))
        size_kb = cover_path.stat().st_size / 1024
        made += 1
        log(f"    ✔ {name}   [{source}] {image.size[0]}x{image.size[1]} -> "
            f"{size}x{size}  {size_kb:.0f} KB")

    return made, len(skipped)


# ============================================================================
# main
# ============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="生成 SD 卡上播放器需要的字库和封面文件")
    parser.add_argument("--music", default=".", help="音乐目录（默认当前目录）")
    parser.add_argument("--out", default="./sd_ready", help="输出目录")
    parser.add_argument("--size", type=int, default=140,
                        help="封面边长，要和固件的 COVER_W 一致（默认 140）")
    parser.add_argument("--no-font", action="store_true", help="跳过字库")
    parser.add_argument("--no-cover", action="store_true", help="跳过封面")
    args = parser.parse_args()

    music_dir = Path(args.music).resolve()
    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    expected_bytes = args.size * args.size * 2
    log(f"音乐目录: {music_dir}")
    log(f"输出目录: {out_dir}")
    log(f"封面尺寸: {args.size}x{args.size} = {expected_bytes} 字节/张")
    log()

    if not args.no_font:
        log("[1/2] 生成中/日文字库")
        generate_font(out_dir)
        log()

    if not args.no_cover:
        log("[2/2] 提取并转换封面")
        made, missing = process_covers(music_dir, out_dir, args.size)
        log()
        log(f"  有封面 {made} 首，没封面 {missing} 首")

    log()
    log("=" * 64)
    log(f"把 {out_dir} 里的所有文件拷到 SD 卡根目录，然后重新上电即可。")
    log("=" * 64)


if __name__ == "__main__":
    sys.exit(main())
