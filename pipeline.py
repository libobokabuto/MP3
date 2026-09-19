"""ncm/mp3 -> SD 卡成品 的增量流水线。

数据流：
    D:\\CloudMusic\\*.mp3                    ┐
    D:\\CloudMusic\\VipSongsDownload\\*.ncm   ┘
                ↓  um.exe 解密（本地编译的官方 unlock-music CLI）
    D:\\software\\Music\\        ← 所有 mp3 汇总（平铺）
                ↓  python prepare_sd.py --state ...
    E:\\Study\\codes\\Mp3\\sd_ready\\  ← font16.bin + 一堆 <歌名>.cov

幂等原则：
    · 每个源文件按「绝对路径」记进 .pipeline_state.json，存 mtime + size + 产出文件指纹
    · 源 mtime/size 没变 且 产出文件还在（mtime/size 也对得上）→ 跳过，不做任何重复工作
    · .cov 和 mp3 写完后，会把 mtime 强制设成和源文件一样 —— 这样「文件指纹」本身就是
      一份可靠的清单，即使清单丢了也能判定"没变过"
    · 源文件（.ncm）永不删除

冲突原则：
    目标目录已存在同名 mp3，而这个文件不在清单里（= 不是本流水线产出的）→ 只打印，
    不覆盖。用 --on-conflict 决定怎么办，默认 skip。

用法：
    python pipeline.py                 # 先看计划，不写盘（dry-run）
    python pipeline.py --yes           # 真正执行
    python pipeline.py --yes --only-ncm
    python pipeline.py --no-sd         # 只汇总 mp3，不跑 prepare_sd
"""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ============================================================================
# 默认路径 / 常量
# ============================================================================

DEFAULT_NCM_DIRS = [
    Path(r"D:\CloudMusic"),
    Path(r"D:\CloudMusic\VipSongsDownload"),
]
DEFAULT_MP3_DIRS = [
    Path(r"D:\CloudMusic"),
]
DEFAULT_TARGET = Path(r"D:\software\Music")
DEFAULT_SD_READY = Path(r"E:\Study\codes\Mp3\sd_ready")
DEFAULT_STATE = Path(r"E:\Study\codes\Mp3\.pipeline_state.json")
DEFAULT_UM = Path(r"D:\software\unlock_music\um.exe")
DEFAULT_PREPARE = Path(r"E:\Study\codes\Mp3\prepare_sd.py")

COVER_EXT = ".cov"
STATE_VERSION = 1

# ============================================================================
# 输出（中文全部从 Python 打印，.cmd 保持纯 ASCII）
# ============================================================================


def log(msg=""):
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        # 万一控制台编码不是 UTF-8，也别让整条流水线崩掉
        enc = sys.stdout.encoding or "ascii"
        print(msg.encode(enc, "replace").decode(enc), flush=True)


def rule(char="-", width=68):
    log(char * width)


def human(size):
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.1f}{unit}" if unit != "B" else f"{size}B"
        size /= 1024


# ============================================================================
# 文件指纹
# ============================================================================


def stat_sig(path):
    """(mtime 取整到秒, size)。NTFS 精度够，取整秒是为了跨工具比较稳定。"""
    st = path.stat()
    return {"mtime": int(st.st_mtime), "size": st.st_size}


def file_ok(path, sig):
    """产出文件还在，而且没被外力改过。"""
    if not path.exists():
        return False
    if sig is None:
        return True
    return stat_sig(path) == sig


def copy_mtime(src, dst):
    """把产出的 mtime 设成源的 mtime —— 让产出自带"我来自哪个版本"的标记。"""
    st = src.stat()
    import os
    os.utime(dst, (st.st_atime, st.st_mtime))


# ============================================================================
# 清单（幂等的大脑）
# ============================================================================


class State:
    def __init__(self, path, read_only=False):
        self.path = Path(path)
        self.read_only = read_only
        self.data = {"version": STATE_VERSION, "items": {}, "font": None,
                     "history": []}
        if self.path.exists():
            try:
                loaded = json.loads(self.path.read_text(encoding="utf-8"))
                if loaded.get("version") == STATE_VERSION:
                    self.data = loaded
                else:
                    log(f"  ⚠ 清单版本不认识（{loaded.get('version')}），"
                        f"当成空清单重来")
            except (OSError, json.JSONDecodeError) as exc:
                log(f"  ⚠ 清单读不出来（{exc}），当成空清单重来")

    @property
    def items(self):
        return self.data["items"]

    def get(self, source_path):
        return self.items.get(str(source_path).lower())

    def put(self, source_path, entry):
        self.items[str(source_path).lower()] = entry

    def save(self):
        if self.read_only:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".json.tmp")
        tmp.write_text(
            json.dumps(self.data, ensure_ascii=False, indent=1),
            encoding="utf-8")
        tmp.replace(self.path)


def outputs_in_state(state):
    """清单里所有已产出的目标文件名（小写）。用来区分"自己产的"和"别人的"。"""
    names = set()
    for entry in state.items.values():
        out = entry.get("output")
        if out:
            names.add(Path(out).name.lower())
    return names


# ============================================================================
# ncm 解析（只读，用来拿封面；不改源文件）
# ============================================================================


class NcmError(Exception):
    pass


def read_ncm(path):
    """解析 .ncm，返回 {format, cover, meta_json}。

    结构（对齐官方 algo/ncm/ncm.go 的 Validate）：
        "CTENFDAM"                   8 字节
        2 字节 gap
        u32 keyLen + keyLen 字节      （AES-128-ECB，key 固定）
        u32 metaLen + metaLen 字节    （base64(AES(…))）
        5 字节 gap
        u32 coverFrameLen
        u32 coverLen + coverLen 字节  ← 明文图片，直接能读
        音频数据
    """
    data = Path(path).read_bytes()
    if data[:8] != b"CTENFDAM":
        raise NcmError("不是 ncm 文件（magic 不对）")

    off = 10
    if off + 4 > len(data):
        raise NcmError("文件被截断")
    key_len = struct.unpack("<I", data[off:off + 4])[0]
    off += 4 + key_len

    if off + 4 > len(data):
        raise NcmError("文件被截断")
    meta_len = struct.unpack("<I", data[off:off + 4])[0]
    off += 4 + meta_len + 5

    if off + 8 > len(data):
        raise NcmError("文件被截断")
    off += 4  # coverFrameLen（含它自己 4 字节 + coverLen 4 字节 + 图片）
    cover_len = struct.unpack("<I", data[off:off + 4])[0]
    off += 4
    cover = data[off:off + cover_len]

    return {"cover": cover}


def sniff_image(raw):
    """返回 (后缀, 宽, 高)，认不出来返回 (None, 0, 0)。"""
    if not raw:
        return None, 0, 0
    if raw[:2] == b"\xff\xd8":
        try:
            from PIL import Image
            import io
            with Image.open(io.BytesIO(raw)) as im:
                return ".jpg", im.size[0], im.size[1]
        except Exception:
            return ".jpg", 0, 0
    if raw[:8] == b"\x89PNG\r\n\x1a\n":
        try:
            w, h = struct.unpack(">II", raw[16:24])
            return ".png", w, h
        except Exception:
            return ".png", 0, 0
    return None, 0, 0


# ============================================================================
# 封面 -> .cov（复用 prepare_sd.py 的转换逻辑，保证两处参数一致）
# ============================================================================


def write_cover_cov(raw_image, out_path, size, prepare_path):
    """把图片写成 <歌名>.cov（140x140 RGB565 小端）。"""
    sys.path.insert(0, str(prepare_path.parent))
    from prepare_sd import image_to_rgb565  # noqa: E402
    import io
    from PIL import Image

    with Image.open(io.BytesIO(raw_image)) as image:
        image.load()
        out_path.write_bytes(image_to_rgb565(image, size))


# ============================================================================
# 各阶段
# ============================================================================


def scan_dir(directory, suffix):
    """列目录里的 *.<suffix>。用 iterdir + suffix.lower()，避免 glob 大小写重复命中。"""
    directory = Path(directory)
    if not directory.is_dir():
        return []
    found = {}
    for entry in directory.iterdir():
        if entry.is_file() and entry.suffix.lower() == suffix.lower():
            found[entry.name.lower()] = entry
    return [found[k] for k in sorted(found)]


def stage_ncm(args, state, plan):
    """把 .ncm 解密成 mp3 放进目标目录。"""
    log("【1/3】ncm 解密")
    rule()

    ncms = []
    for directory in args.ncm_dirs:
        ncms.extend(scan_dir(directory, ".ncm"))

    if not ncms:
        log("  没有找到 .ncm，跳过")
        log()
        return

    log(f"  扫描到 {len(ncms)} 个 .ncm")
    log()

    um = Path(args.um)
    if not args.dry_run and not um.exists():
        log(f"  ✘ 找不到转换工具：{um}")
        log("    先编译：见 D:\\software\\unlock_music\\（源码已就位）")
        raise SystemExit(2)

    known_outputs = outputs_in_state(state)
    target = Path(args.target)

    for ncm in ncms:
        sig = stat_sig(ncm)
        entry = state.get(ncm)
        name = ncm.stem
        ext = (entry or {}).get("ext", ".mp3")
        dest = target / (name + ext)

        # ---- 判定 1：这条流水线自己处理过，且源没变、产出还在 ----
        if entry and entry.get("source_sig") == sig:
            out = Path(entry.get("output", ""))
            if out and file_ok(out, entry.get("output_sig")):
                plan["skip"].append((name, str(out)))
                continue

        # ---- 判定 2：目标目录里已经有这个文件，但清单里没记录 = 不是我们产的 ----
        foreign = dest.exists() and dest.name.lower() not in known_outputs

        if foreign and args.on_conflict == "skip":
            plan["conflict"].append((name, str(dest)))
            continue

        if args.dry_run:
            plan["new"].append((name, str(dest)))
            continue

        # ---- 真转换（tmp 目录必须在 with 里面用完，否则文件已经没了）----
        target.mkdir(parents=True, exist_ok=True)
        produced = None
        with tempfile.TemporaryDirectory(prefix="um_") as tmp:
            produced = run_um(um, ncm, Path(tmp), args)
            if produced is None:
                plan.setdefault("fail", []).append((name, "um 转换失败"))
                continue

            ext = produced.suffix
            target_path = target / (name + ext)

            # 重名处理
            if target_path.exists():
                if foreign and args.on_conflict == "rename":
                    backup = target_path.with_suffix(target_path.suffix + ".bak")
                    if not backup.exists():
                        shutil.move(str(target_path), str(backup))
                elif foreign and args.on_conflict == "skip":
                    plan["conflict"].append((name, str(target_path)))
                    continue
                # overwrite，或非冲突（清单里认得的旧版自产文件）→ 直接替换

            shutil.move(str(produced), str(target_path))
        copy_mtime(ncm, target_path)

        # ---- 抠封面（ncm 里的封面是明文，不用 ffmpeg）----
        cover_note = "无封面"
        try:
            raw = read_ncm(ncm)["cover"]
            img_ext, w, h = sniff_image(raw)
            if img_ext:
                cov = Path(args.sd_ready) / (name + COVER_EXT)
                Path(args.sd_ready).mkdir(parents=True, exist_ok=True)
                write_cover_cov(raw, cov, args.cover_size, Path(args.prepare))
                copy_mtime(ncm, cov)
                cover_note = f"封面 {w}x{h}"
        except (NcmError, OSError, ValueError, ImportError) as exc:
            cover_note = f"封面提取失败（{exc}）"

        state.put(ncm, {
            "kind": "ncm",
            "source_sig": sig,
            "output": str(target_path),
            "output_sig": stat_sig(target_path),
            "ext": ext,
            "cover": cover_note,
            "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        })
        plan["done"].append(
            (name, f"{ext}  {human(target_path.stat().st_size)}  {cover_note}"))

    log()


def run_um(um, ncm, out_dir, args):
    """跑 um.exe，返回产出文件路径；失败返回 None。"""
    cmd = [str(um), "-o", str(out_dir), str(ncm)]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        log(f"    ! {ncm.name}  超时（{args.timeout}s）")
        return None
    except OSError as exc:
        log(f"    ! {ncm.name}  启动失败：{exc}")
        return None

    produced = [p for p in Path(out_dir).iterdir() if p.is_file()]
    text = (proc.stdout or b"").decode("utf-8", "replace")
    if proc.returncode != 0 or not produced:
        tail = text.strip().splitlines()[-1:] or ["（无输出）"]
        log(f"    ! {ncm.name}  转换失败：{tail[0]}")
        return None
    if "skip" in text and "already exist" in text:
        log(f"    ! {ncm.name}  um 说目标已存在，跳过")
        return None
    return produced[0]


def stage_mp3(args, state, plan):
    """把 CloudMusic 里的散装 mp3 汇总到目标目录。"""
    log("【2/3】mp3 汇总")
    rule()

    mp3s = []
    for directory in args.mp3_dirs:
        mp3s.extend(scan_dir(directory, ".mp3"))
        mp3s.extend(scan_dir(directory, ".flac"))

    if not mp3s:
        log("  没有找到需要汇总的 mp3，跳过")
        log("  （要汇总的歌直接丢进上面那些源目录即可，下次跑就会带上）")
        log()
        return

    target = Path(args.target)
    log(f"  扫描到 {len(mp3s)} 个文件")
    known_outputs = outputs_in_state(state)

    for src in mp3s:
        sig = stat_sig(src)
        entry = state.get(src)
        dst = target / src.name

        if entry and entry.get("source_sig") == sig:
            out = Path(entry.get("output", ""))
            if out and file_ok(out, entry.get("output_sig")):
                plan["skip"].append((src.name, "已汇总"))
                continue

        conflict = None
        if dst.exists() and dst.name.lower() not in known_outputs:
            conflict = dst

        if conflict and args.on_conflict == "skip":
            plan["conflict"].append((src.name, str(dst)))
            continue

        plan["new" if not conflict else "conflict"].append((src.name, str(dst)))

        if args.dry_run:
            continue

        if dst.exists():
            if args.on_conflict == "rename":
                backup = dst.with_suffix(dst.suffix + ".bak")
                if not backup.exists():
                    shutil.move(str(dst), str(backup))
            # overwrite：shutil.copy2 自己会盖

        target.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        copy_mtime(src, dst)

        state.put(src, {
            "kind": "mp3",
            "source_sig": sig,
            "output": str(dst),
            "output_sig": stat_sig(dst),
            "when": time.strftime("%Y-%m-%d %H:%M:%S"),
        })
        plan["done"].append((src.name, f"{human(dst.stat().st_size)}"))

    log()


def stage_sd(args, state, plan):
    """调 prepare_sd.py 出字库 + 封面。"""
    log("【3/3】生成字库和封面")
    rule()

    if args.no_sd:
        log("  已按 --no-sd 跳过")
        log()
        return

    prepare = Path(args.prepare)
    if not prepare.exists():
        log(f"  ✘ 找不到 {prepare}")
        raise SystemExit(2)

    cmd = [sys.executable, str(prepare),
           "--music", str(args.target),
           "--out", str(args.sd_ready),
           "--size", str(args.cover_size),
           "--state", str(args.state)]
    if args.force:
        cmd.append("--force")

    if args.dry_run:
        cmd.append("--dry-run")

    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        log(f"  ✘ prepare_sd.py 退出码 {proc.returncode}")
        plan.setdefault("fail", []).append(("prepare_sd", f"退出码 {proc.returncode}"))

    # prepare_sd.py 是子进程，它写进清单的东西要并回来。
    # ⚠ 这里必须【按 key 合并】，不能整体替换 —— 我们内存里的条目在磁盘上
    #   可能还不存在（见下面 stage 结束处的 state.save()，那是修过的坑）。
    fresh = State(args.state)
    for key, entry in fresh.items.items():
        state.items.setdefault(key, entry)
    if fresh.data.get("font"):
        state.data["font"] = fresh.data["font"]
    log()


# ============================================================================
# 汇总
# ============================================================================


def print_plan(plan, args):
    rule("=")

    if plan["done"]:
        log(f"  新增/更新 {len(plan['done'])} 首：")
        for name, note in plan["done"]:
            log(f"    新增  {name}    {note}")
        log()

    if plan["new"] and args.dry_run:
        log(f"  将要新增 {len(plan['new'])} 首：")
        for name, dest in plan["new"]:
            log(f"    新增  {name}")
        log()

    if plan["skip"]:
        log(f"  跳过 {len(plan['skip'])} 首（源没变，产出还在）：")
        for name, _ in plan["skip"][:6]:
            log(f"    跳过  {name}")
        if len(plan["skip"]) > 6:
            log(f"    …… 其余 {len(plan['skip']) - 6} 首同样跳过")
        log()

    if plan["conflict"]:
        log(f"  ⚠ 冲突 {len(plan['conflict'])} 首（目标已有同名文件，且不是本流水线产的）：")
        for name, dest in plan["conflict"]:
            log(f"    冲突  {name}")
            log(f"          → 已存在：{dest}")
        log("    这些【没有动】。要处理请加参数：")
        log("      --on-conflict=rename      旧文件改名加 .bak，再写新的")
        log("      --on-conflict=overwrite   直接覆盖（危险）")
        log()

    if plan.get("fail"):
        log(f"  ✘ 失败 {len(plan['fail'])} 项：")
        for name, why in plan["fail"]:
            log(f"    失败  {name}    {why}")
        log()

    total = len(plan["done"]) + len(plan["new"]) + len(plan["skip"]) + len(plan["conflict"])
    rule()
    log(f"  合计 {total} 首：新增 {len(plan['done']) or len(plan['new'])} / "
        f"跳过 {len(plan['skip'])} / 冲突 {len(plan['conflict'])} / "
        f"失败 {len(plan.get('fail', []))}")
    rule("=")


def report_track_cap(target):
    """固件只扫 64 首，超了要提醒（只提醒，不动文件）。"""
    mp3s = scan_dir(target, ".mp3")
    log(f"  目标目录现有 {len(mp3s)} 首 mp3（固件 MAX_TRACKS = 64）")
    if len(mp3s) > 64:
        log(f"  ⚠ 超过 64 首了，播放器只会列出前 64 首。")
        log(f"    处理办法：把不听的挪出 {target}，或等固件 v2.5 扩容。")


def run_all_stages(args):
    """按顺序跑三个阶段，返回本次的计划。三个阶段只在这里串一次。"""
    state = State(args.state, read_only=args.dry_run)
    plan = {"new": [], "done": [], "skip": [], "conflict": [], "fail": []}

    stage_ncm(args, state, plan)
    if not args.dry_run:
        state.save()   # ⚠ 必须立刻落盘：prepare_sd.py 是子进程，它要读这个清单

    if args.only_ncm:
        log("（--only-ncm：跳过 mp3 汇总和字库封面）")
        log()
        return plan

    stage_mp3(args, state, plan)
    if not args.dry_run:
        state.save()

    stage_sd(args, state, plan)

    if not args.dry_run:
        state.data["last_run"] = time.strftime("%Y-%m-%d %H:%M:%S")
        state.save()

    return plan


def ask_to_continue():
    """问一句再动手。双击运行的入口用这个，避免误触就写盘。"""
    log("  ⚠ 上面就是即将发生的全部改动。")
    log("    回车 = 开始写盘；输入 n 再回车 = 取消。")
    log()
    try:
        answer = input("  继续吗？ [回车=继续 / n=取消]  ")
    except (EOFError, KeyboardInterrupt):
        log()
        return False
    return answer.strip().lower() not in ("n", "no", "不", "取消", "q", "quit")


# ============================================================================
# main
# ============================================================================


def main():
    parser = argparse.ArgumentParser(
        description="ncm/mp3 -> SD 卡成品 的增量流水线")
    parser.add_argument("--ncm-dir", action="append", default=None,
                        help="ncm 源目录（可重复；默认 CloudMusic 和 VipSongsDownload）")
    parser.add_argument("--mp3-dir", action="append", default=None,
                        help="散装 mp3 源目录（可重复；默认 CloudMusic）")
    parser.add_argument("--target", default=str(DEFAULT_TARGET),
                        help=f"mp3 汇总目录（默认 {DEFAULT_TARGET}）")
    parser.add_argument("--sd-ready", default=str(DEFAULT_SD_READY),
                        help=f"字库/封面产出目录（默认 {DEFAULT_SD_READY}）")
    parser.add_argument("--state", default=str(DEFAULT_STATE),
                        help=f"清单文件（默认 {DEFAULT_STATE}）")
    parser.add_argument("--um", default=str(DEFAULT_UM),
                        help=f"unlock-music CLI（默认 {DEFAULT_UM}）")
    parser.add_argument("--prepare", default=str(DEFAULT_PREPARE),
                        help=f"prepare_sd.py 路径（默认 {DEFAULT_PREPARE}）")
    parser.add_argument("--size", type=int, default=140, dest="cover_size",
                        help="封面边长，要和固件 COVER_W 一致（默认 140）")
    parser.add_argument("--on-conflict", choices=("skip", "rename", "overwrite"),
                        default="skip",
                        help="目标同名文件怎么办（默认 skip：只报告不动它）")
    parser.add_argument("--timeout", type=int, default=180,
                        help="单个 ncm 转换超时秒数（默认 180）")
    parser.add_argument("--yes", action="store_true",
                        help="真正写盘。不加这个参数就是 dry-run，只打印计划")
    parser.add_argument("--confirm", action="store_true",
                        help="先打计划 → 问一句「继续吗」→ 回车才写盘（双击入口用这个）")
    parser.add_argument("--force", action="store_true",
                        help="忽略清单，全部重做（包括字库）")
    parser.add_argument("--no-sd", action="store_true",
                        help="只汇总 mp3，不跑 prepare_sd.py")
    parser.add_argument("--only-ncm", action="store_true",
                        help="只做 ncm 解密，不汇总 mp3、不跑 prepare_sd.py")
    args = parser.parse_args()

    args.dry_run = not args.yes
    args.ncm_dirs = [Path(p) for p in (args.ncm_dir or DEFAULT_NCM_DIRS)]
    args.mp3_dirs = [Path(p) for p in (args.mp3_dir or DEFAULT_MP3_DIRS)]

    # 三种模式：
    #   --yes              直接写盘
    #   --confirm          先打计划 → 问一句 → 回车才写盘（双击入口用这个）
    #   （都不给）          纯预演，只看计划
    force_requested = args.force

    rule("=")
    log("  MP3 播放器 —— ncm/mp3 -> SD 卡 增量流水线")
    rule("=")
    log()
    log(f"  ncm 源目录 : {', '.join(str(p) for p in args.ncm_dirs)}")
    log(f"  mp3 源目录 : {', '.join(str(p) for p in args.mp3_dirs)}")
    log(f"  汇总目录   : {args.target}")
    log(f"  产出目录   : {args.sd_ready}")
    log(f"  清单文件   : {args.state}")
    log(f"  转换工具   : {args.um}")
    log()

    missing = [str(p) for p in args.ncm_dirs if not Path(p).is_dir()]
    if missing:
        log(f"  提示：这些 ncm 源目录不存在，跳过：{', '.join(missing)}")
        log()
    for p in list(args.mp3_dirs):
        if not Path(p).is_dir():
            log(f"  提示：mp3 源目录不存在，跳过：{p}")
            log()

    # ---- 第一趟：预演（绝不写盘）----
    args.dry_run = True
    args.force = False
    if args.confirm:
        log("  ── 第一步：先看计划（这一步不写任何文件）──")
        log()
    else:
        log("  ⚠ 当前是【预演模式】—— 只看计划，一个字节都不会写。")
        log("    确认没问题后加 --yes 真正执行。")
        log()

    preview_plan = run_all_stages(args)
    print_plan(preview_plan, args)
    report_track_cap(Path(args.target))
    preview_fail = bool(preview_plan.get("fail"))

    if not args.confirm:
        log()
        rule("=")
        log("  预演结束，什么都没写。确认无误后加 --yes 真正执行。")
        rule("=")
        return 1 if preview_fail else 0

    # ---- 问一句 ----
    log()
    rule("=")
    if not ask_to_continue():
        log("  已取消，什么都没写。")
        rule("=")
        return 0

    # ---- 第二趟：真正执行 ----
    log()
    rule("=")
    log("  ── 第二步：开始执行（会写入上面两个目录）──")
    rule("=")
    log()

    args.dry_run = False
    args.force = force_requested

    started = time.time()
    plan = run_all_stages(args)
    print_plan(plan, args)
    report_track_cap(Path(args.target))

    elapsed = time.time() - started
    log()
    log(f"  耗时 {elapsed:.1f} 秒")
    rule("=")
    log("  下一步：把下面两处的东西【全部平铺】拷到 SD 卡根目录")
    log(f"    · {args.target}   里的 .mp3")
    log(f"    · {args.sd_ready}   里的 font16.bin + 所有 .cov")
    rule("=")

    return 1 if plan.get("fail") else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        log()
        log("  已中断。已经写完的文件不会回滚，下次跑会自动跳过它们。")
        sys.exit(130)
    except SystemExit as exc:
        # 双击运行时别丢一堆 traceback，给个人话
        if exc.code:
            log()
            log(f"  中止（退出码 {exc.code}）。上面有原因，改完再跑一次即可。")
        sys.exit(exc.code)
