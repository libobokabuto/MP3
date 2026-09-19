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
import json
import re
import struct
import sys
import time
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

# ============================================================================
# 默认路径
# ============================================================================
#
# 工作方式：读左边，写右边。
#   D:\software\Music          放 mp3（平铺就行，播放器只扫 SD 卡根目录，不支持子文件夹）
#   E:\Study\codes\Mp3\sd_ready  放产出（font16.bin + 一堆 <歌名>.cov）
#
# 直接跑  python prepare_sd.py  或双击 一键备卡.cmd 回车，用的就是这两个目录。
# 想临时换：命令行加 --music / --out，或者在交互式里直接输入路径。

DEFAULT_MUSIC_DIR = Path(r"D:\software\Music")
DEFAULT_OUT_DIR = Path(r"E:\Study\codes\Mp3\sd_ready")
DEFAULT_STATE_FILE = Path(r"E:\Study\codes\Mp3\.pipeline_state.json")


# ============================================================================
# 增量支持
# ============================================================================
#
# 清单文件由本脚本和 pipeline.py 共用，格式：
#   { "version": 1,
#     "items": { "<源文件绝对路径小写>": {
#         "source_sig": {"mtime": 秒, "size": 字节},   源文件指纹
#         "output":     "<产出文件路径>",
#         "output_sig": {"mtime": 秒, "size": 字节} } },
#     "font": {...} }
#
# 判定规则：源没变 且 产出还在（指纹也对得上）→ 跳过，不做重复工作。
# 产出写完会把 mtime 设成和源一样，所以即使清单丢了，"文件本身"也能说明问题。

STATE_VERSION = 1


def log(msg=""):
    print(msg, flush=True)


def stat_sig(path):
    st = Path(path).stat()
    return {"mtime": int(st.st_mtime), "size": st.st_size}


def copy_mtime(src, dst):
    import os
    st = Path(src).stat()
    os.utime(dst, (st.st_atime, st.st_mtime))


class State:
    def __init__(self, path, read_only=False):
        self.path = Path(path) if path else None
        self.read_only = read_only or self.path is None
        self.data = {"version": STATE_VERSION, "items": {}, "font": None}
        if self.path and self.path.exists():
            try:
                loaded = json.loads(self.path.read_text(encoding="utf-8"))
                if loaded.get("version") == STATE_VERSION:
                    self.data = loaded
                    self.data.setdefault("items", {})
                else:
                    log(f"  ⚠ 清单版本不认识，忽略旧清单")
            except (OSError, json.JSONDecodeError) as exc:
                log(f"  ⚠ 清单读不出来（{exc}），忽略旧清单")

    def get(self, source_path):
        return self.data["items"].get(str(source_path).lower())

    def put(self, source_path, entry):
        self.data["items"][str(source_path).lower()] = entry

    def save(self):
        if self.read_only:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, indent=1),
                       encoding="utf-8")
        tmp.replace(self.path)


def font_fingerprint():
    """字库的"输入指纹"：区段配置 + 两个字体文件本身。改任何一个都能察觉到。"""
    parts = [f"{FONT_MAGIC!r}", GLYPH_W, GLYPH_H,
             ";".join(f"{s:x}-{e:x}-{w}" for s, e, w in FONT_RANGES)]
    for candidates in (HANZI_FONT_CANDIDATES, KANA_FONT_CANDIDATES):
        for path in candidates:
            if Path(path).exists():
                parts.append(f"{path}:{stat_sig(path)['mtime']}:"
                             f"{stat_sig(path)['size']}")
                break
        else:
            parts.append("none")
    return "|".join(str(p) for p in parts)


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


def generate_font(out_dir, state=None, force=False, dry_run=False):
    out_path = out_dir / FONT_FILE
    fingerprint = font_fingerprint()

    # 增量：字库配置和字体文件都没变、产出还在 → 跳过（这是最省时间的一步，
    # 全量重算要遍历 21696 个字形）
    if state is not None and not force:
        recorded = state.data.get("font")
        if (recorded and recorded.get("fingerprint") == fingerprint
                and out_path.exists()
                and stat_sig(out_path) == recorded.get("output_sig")):
            log(f"  · 跳过（字体和区段都没变）  {out_path.name}  "
                f"{out_path.stat().st_size / 1024:.0f} KB")
            return out_path

    if dry_run:
        log(f"  + 需要重建  {out_path.name}")
        return out_path

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
    tmp_path = out_path.with_suffix(".bin.tmp")
    with open(tmp_path, "wb") as f:
        f.write(FONT_MAGIC)
        f.write(struct.pack("<HHHHI", GLYPH_W, GLYPH_H, BYTES_PER_GLYPH,
                            len(FONT_RANGES), 0))
        for start, end, offset in segment_headers:
            f.write(struct.pack("<III", start, end, offset))
        f.write(body)
    tmp_path.replace(out_path)

    size_kb = out_path.stat().st_size / 1024
    log(f"  ✔ {out_path}  ({size_kb:.0f} KB)")
    if blank_count:
        log(f"  ⚠ 有 {blank_count} 个字形在该字体里是空白的（属正常，冷门码位）")

    if state is not None:
        state.data["font"] = {"fingerprint": fingerprint,
                              "output_sig": stat_sig(out_path),
                              "when": time.strftime("%Y-%m-%d %H:%M:%S")}
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


def report_where_the_songs_are(music_dir):
    """目录里没找到 mp3 时，帮忙查一下为什么。"""
    log(f"  ⚠ {music_dir} 里没有 .mp3")

    ncm_count = sum(1 for entry in music_dir.iterdir()
                    if entry.is_file() and entry.suffix.lower() == ".ncm")

    if ncm_count:
        log()
        log(f"     这里有 {ncm_count} 个 .ncm 文件。")
        log("     .ncm 是云音乐的加密格式，必须先转成 .mp3 播放器才认。")


def write_atomic(path, data):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(data)
    tmp.replace(path)


def cover_input_signature(mp3):
    """封面"输入"的指纹 —— 决定这张 .cov 该不该重做。

    返回 (source, sig, note)：
      · 内嵌封面：源指纹就是 mp3 自己（mtime+size）+ 尺寸，mp3 没变就不重抠
      · 同名图片：源指纹是那张图，换图能察觉到
      · 没有封面：返回 (None, None, 原因)
    """
    raw = extract_embedded_cover(mp3)
    if raw is not None:
        sig = stat_sig(mp3)
        return "内嵌", sig, "内嵌"

    sidecar = find_sidecar_image(mp3)
    if sidecar is None:
        return None, None, "没有封面"

    sig = stat_sig(sidecar)
    return "同名图片", sig, sidecar.suffix


def process_covers(music_dir, out_dir, size, state=None, force=False,
                   dry_run=False):
    mp3_files = list_mp3_files(music_dir)
    if not mp3_files:
        report_where_the_songs_are(music_dir)
        return {"made": 0, "skip": 0, "no_cover": 0, "pending": 0}

    made = skip = no_cover = pending = 0

    for mp3 in mp3_files:
        name = mp3.stem
        cover_path = out_dir / (name + COVER_EXT)

        # ---- 快路径：先查清单，命中就【连 MP3 都不用读】 ----
        # 这一步是"第二次跑秒级完成"的关键：跳过时不做任何 I/O 解码。
        if state is not None and not force:
            recorded = state.get(mp3)
            if (recorded
                    and recorded.get("kind") == "cover"
                    and recorded.get("source_sig") == stat_sig(mp3)
                    and recorded.get("size") == size):
                if recorded.get("note") == "no-cover":
                    no_cover += 1
                    log(f"    - {name}   没有封面（上次已确认）")
                    continue
                if (cover_path.exists()
                        and stat_sig(cover_path) == recorded.get("output_sig")):
                    skip += 1
                    log(f"    · 跳过  {name}")
                    continue

        source, sig, note = cover_input_signature(mp3)

        # ---- 已经被流水线产好了？（ncm 的封面来自 .ncm 内部，mp3 里没内嵌）----
        # pipeline.py 会先把 .cov 写到 out_dir，我们认下来并记账，不重复劳动。
        if source is None and cover_path.exists():
            expected = size * size * 2
            if cover_path.stat().st_size == expected:
                if state is not None and not dry_run:
                    state.put(mp3, {
                        "kind": "cover", "source_sig": stat_sig(mp3), "size": size,
                        "output": str(cover_path),
                        "output_sig": stat_sig(cover_path),
                        "note": "pre-made",
                        "when": time.strftime("%Y-%m-%d %H:%M:%S")})
                skip += 1
                log(f"    · 跳过  {name}   （封面已由流水线产好）")
                continue

        if source is None:
            no_cover += 1
            log(f"    - {name}   没有封面")
            if state is not None and not dry_run:
                state.put(mp3, {"kind": "cover", "source_sig": stat_sig(mp3),
                                "size": size, "output": "", "note": "no-cover",
                                "when": time.strftime("%Y-%m-%d %H:%M:%S")})
            continue

        if dry_run:
            pending += 1
            log(f"    + 需要重做  {name}   [{note}]")
            continue

        raw = extract_embedded_cover(mp3)
        source_label = "内嵌"
        if raw is None:
            sidecar = find_sidecar_image(mp3)
            raw = sidecar.read_bytes()
            source_label = sidecar.suffix

        try:
            image = Image.open(io.BytesIO(raw))
            image.load()
        except Exception as exc:
            no_cover += 1
            log(f"    ! {name}   图片打不开（{exc}）")
            continue

        write_atomic(cover_path, image_to_rgb565(image, size))
        if source == "同名图片":
            copy_mtime(find_sidecar_image(mp3), cover_path)
        else:
            copy_mtime(mp3, cover_path)

        size_kb = cover_path.stat().st_size / 1024
        made += 1
        log(f"    ✔ {name}   [{source_label}] {image.size[0]}x{image.size[1]} -> "
            f"{size}x{size}  {size_kb:.0f} KB")

        if state is not None:
            state.put(mp3, {"kind": "cover", "source_sig": sig, "size": size,
                            "output": str(cover_path),
                            "output_sig": stat_sig(cover_path),
                            "when": time.strftime("%Y-%m-%d %H:%M:%S")})

    return {"made": made, "skip": skip, "no_cover": no_cover,
            "pending": pending}


# ============================================================================
# main
# ============================================================================


def ask_for_target():
    """交互式问用户要处理哪个目录。给双击运行的 一键备卡.cmd 用。

    所有中文都从 Python 输出，而不是写在 .cmd 里 ——
    cmd.exe 解析 UTF-8 批处理文件不可靠，中文行会被当成命令报错。
    """
    print("=" * 64)
    print("  MP3 播放器 —— 一键备卡")
    print("=" * 64)
    print()
    print("  直接回车 = 用下面的默认设置：")
    print()
    print(f"    歌曲目录（读）: {DEFAULT_MUSIC_DIR}")
    print(f"    输出目录（写）: {DEFAULT_OUT_DIR}")
    print()
    print("  也可以输入：")
    print("    · SD 卡盘符（比如 F）  —— 直接对卡操作，字库和封面原地写进卡里")
    print("    · 别的音乐文件夹路径    —— 输出仍然去上面那个输出目录")
    print()
    print("  ⚠ 播放器只扫 SD 卡的【根目录】，不支持子文件夹。")
    print("    所以 mp3 要放在同一个文件夹里，不要分很多层。")
    print()

    try:
        raw = input("  请输入（直接回车 = 用默认设置）: ")
    except (EOFError, KeyboardInterrupt):
        print()
        return None

    raw = raw.strip().strip('"').strip()

    # 直接回车 -> 用默认的一读一写
    if not raw:
        return ("default", None)

    # 只输了一个字母 -> 当成盘符
    if re.fullmatch(r"[A-Za-z]", raw.rstrip(":\\/")):
        return ("drive", raw.rstrip(":\\/").upper())

    return ("dir", raw)


def normalize_dir(value):
    """把用户给的各种写法统一成绝对目录路径。

    专门处理 Windows 命令行的一个坑：
    在 cmd 里写  --music "F:\\"  时，C 运行时会认为反斜杠把引号转义了，
    Python 实际收到的是  F:"  （末尾多一个引号），于是找不到盘。
    所以这里先剥掉多余的引号，再把"只有一个盘符"补成盘根。
    """
    value = value.strip()
    while value.endswith('"'):
        value = value[:-1]

    if re.fullmatch(r"[A-Za-z]:", value):
        value += "\\"

    return Path(value).resolve()


def main():
    parser = argparse.ArgumentParser(
        description="生成 SD 卡上播放器需要的字库和封面文件")
    parser.add_argument("--music", default=None,
                        help=f"音乐目录（默认 {DEFAULT_MUSIC_DIR}）")
    parser.add_argument("--out", default=None,
                        help=f"输出目录（默认 {DEFAULT_OUT_DIR}）")
    parser.add_argument("--size", type=int, default=140,
                        help="封面边长，要和固件的 COVER_W 一致（默认 140）")
    parser.add_argument("--no-font", action="store_true", help="跳过字库")
    parser.add_argument("--no-cover", action="store_true", help="跳过封面")
    parser.add_argument("--state", default=None,
                        help="增量清单文件（给 pipeline.py 用；不给就不做增量记录）")
    parser.add_argument("--default-state", action="store_true",
                        help="用默认清单路径（给 一键备卡.cmd 用）")
    parser.add_argument("--force", action="store_true",
                        help="忽略清单，字库和封面全部重做")
    parser.add_argument("--dry-run", action="store_true",
                        help="只说要做什么，不写盘")
    parser.add_argument("-i", "--interactive", action="store_true",
                        help="交互式提问（给双击运行的 .cmd 用，回车=用默认设置）")
    args = parser.parse_args()

    if args.interactive:
        target = ask_for_target()
        if not target:
            print("  已取消。")
            return 1

        kind, value = target

        if kind == "default":
            args.music = str(DEFAULT_MUSIC_DIR)
            args.out = str(DEFAULT_OUT_DIR)
            print(f"\n  歌曲目录：{args.music}")
            print(f"  输出目录：{args.out}\n")
        elif kind == "drive":
            if not Path(f"{value}:/").exists():
                print()
                print(f"  找不到 {value}: 盘。检查一下：")
                print("    1) 盘符字母对不对（打开「此电脑」看那张卡的字母）")
                print("    2) 卡有没有插好、读卡器有没有被识别")
                return 1
            args.music = f"{value}:/"
            args.out = f"{value}:/"
            print(f"\n  歌曲目录：{value}:\\  （SD 卡）")
            print(f"  输出目录：{value}:\\  （原地读写）\n")
        else:
            if not Path(value).is_dir():
                print()
                print(f"  找不到这个文件夹：{value}")
                print("  检查一下路径有没有打错。")
                return 1
            args.music = value
            args.out = str(DEFAULT_OUT_DIR)
            print(f"\n  歌曲目录：{value}")
            print(f"  输出目录：{DEFAULT_OUT_DIR}\n")

    if args.music is None:
        args.music = str(DEFAULT_MUSIC_DIR)
    if args.out is None:
        args.out = str(DEFAULT_OUT_DIR)

    try:
        music_dir = normalize_dir(args.music)
        out_dir = normalize_dir(args.out)
    except OSError as exc:
        raise SystemExit(f"路径解析失败：{exc}")

    if not music_dir.is_dir():
        raise SystemExit(f"找不到音乐目录：{music_dir}\n"
                         f"（确认盘符对不对、卡插好了没有）")

    out_dir.mkdir(parents=True, exist_ok=True)

    # 增量清单：给路径就用；--default-state 用默认路径；
    # 直接双击 一键备卡.cmd 时自动打开默认清单（这样日常操作也变快）
    if args.state is None and args.default_state:
        args.state = str(DEFAULT_STATE_FILE)
    state = State(args.state, read_only=args.dry_run)
    if args.state and not args.dry_run:
        log(f"增量清单: {args.state}")

    if args.dry_run:
        log("⚠ 预演模式：只看要做什么，不写任何文件")

    expected_bytes = args.size * args.size * 2
    log(f"音乐目录: {music_dir}")
    log(f"输出目录: {out_dir}")
    log(f"封面尺寸: {args.size}x{args.size} = {expected_bytes} 字节/张")
    log()

    if not args.no_font:
        log("[1/2] 生成中/日文字库")
        generate_font(out_dir, state, args.force, args.dry_run)
        log()

    if not args.no_cover:
        log("[2/2] 提取并转换封面")
        stats = process_covers(music_dir, out_dir, args.size, state,
                               args.force, args.dry_run)
        log()
        log(f"  做了 {stats['made']} 首，跳过 {stats['skip']} 首，"
            f"没封面 {stats['no_cover']} 首"
            + (f"，待做 {stats['pending']} 首" if stats['pending'] else ""))

    if not args.dry_run:
        state.save()

    log()
    log("=" * 64)

    if music_dir == out_dir and out_dir.parent == out_dir:
        # 写进盘根 = 直接写在 SD 卡上
        log("完成！文件已经直接写进 SD 卡了。")
        log("把卡拔下来插回播放器，重新上电即可。")
    elif music_dir == out_dir:
        # 电脑上的文件夹：mp3 和封面在一起，整套拷过去
        log(f"完成！现在把 {out_dir} 里的东西【全部】拷到 SD 卡根目录：")
        log("    · 你的 .mp3")
        log("    · font16.bin")
        log("    · 所有的 .cov")
        log()
        log("注意：全都放在 SD 卡根目录，不要建子文件夹。")
    else:
        log("完成！现在把这两边的东西都拷到 SD 卡根目录：")
        log()
        log(f"  封面 + 字库：{out_dir}")
        log(f"                 （这个目录里的全部文件）")
        log(f"  歌曲：      {music_dir}")
        log(f"                 （只要 .mp3）")
        log()
        log("注意：全部平铺在 SD 卡根目录，不要建子文件夹。")

    log("=" * 64)
    return 0


if __name__ == "__main__":
    sys.exit(main())
