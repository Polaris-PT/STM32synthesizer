#!/usr/bin/env python3
"""pack_samples.py —— 把 PC 上的 WAV 采样打包成板子用的采样库 samples.bin。

板子侧流程：samples.bin 拷到 SD 卡 /synth/ 下 → 上电时（或发 PC 时）固件把它写进
QSPI flash，之后按 Program Change 零拷贝播放。格式定义见 firmware
`Synth/port/sample_storage_qspi.c`（version 2）。

目录约定（默认源目录 ./samples）：

    samples/
      manifest.json          # 可选，见下
      piano/C4.wav  piano/Ds4.wav  piano/Fs4.wav ...
      strings/C3.wav ...
      perc/kick.wav  perc/snare.wav ...     # 没有音高名的当"单音"处理（root 由 manifest 指定）

manifest.json（可选，缺省时目录名当乐器名、PC 按 20/21/22… 分配）：

    {
      "instruments": [
        {"dir": "piano", "pc": 20, "name": "Piano",  "volume": 230, "loop": false},
        {"dir": "strings","pc": 21, "name": "Strings","volume": 200, "loop": true,
         "root": 60, "mismatch_limit": 7},
        {"dir": "perc",   "pc": 30, "name": "Perc",   "volume": 255, "roots": {"kick": 36, "snare": 38}}
      ]
    }

用法：
    python tools/pack_samples.py                      # 打包 ./samples → ./samples.bin
    python tools/pack_samples.py --list samples.bin   # 看已有库的内容
    python tools/pack_samples.py --selftest           # 生成固件自检用的同款小库（验证通路）
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import wave
import zlib
from dataclasses import dataclass, field

# ---- 与固件一致的常量（改这里之前先看 Synth/port/sample_storage_qspi.c）----
MAGIC = 0x42494C53          # 'SLIB'
VERSION = 2
DESC_BYTES = 0x4000         # 头 + 乐器表 + 区表都在这 16 KB 里
DATA_BASE = 0x4000          # 采样数据从库内偏移 0x4000 开始
DATA_LIMIT = 0x7FF000       # 最后一个 4 KB 扇区留给 QSPI 写测试，别占
QSPI_SIZE = 8 * 1024 * 1024
MAX_NAME = 15               # name[16]，含结尾 0

HDR_FMT = "<IHHIIIIII4s"    # magic version hdr_bytes instr zone data_off data_bytes serial crc reserved[4]
INSTR_FMT = "<16sBBBBHH"
ZONE_FMT = "<IIIII4B4BHH"   # offset frames rate loop_start loop_end root low high flags interp pad[3] gain_q8 crossfade

# 固件里 hdr_bytes = sizeof(它的头结构体)，所以这里必须用打包后的真实长度（36），不能想当然写 32
HDR_BYTES = struct.calcsize(HDR_FMT)

NOTE_BASE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_from_name(text: str) -> int | None:
    """'C4' / 'Fs3' / 'A#2' / 'Db4' / '60' → MIDI 音符号（C4 = 60）。"""
    text = text.strip()
    if text.isdigit():
        v = int(text)
        return v if 0 <= v <= 127 else None
    if not text:
        return None
    letter = text[0].upper()
    if letter not in NOTE_BASE:
        return None
    i, acc = 1, 0
    if i < len(text) and text[i] in "#sS":
        acc, i = 1, i + 1
    elif i < len(text) and text[i] in "bB":
        acc, i = -1, i + 1
    if i >= len(text):
        return None
    try:
        octave = int(text[i:])
    except ValueError:
        return None
    midi = NOTE_BASE[letter] + acc + (octave + 1) * 12   # C4 = 60 ⇒ octave+1
    return midi if 0 <= midi <= 127 else None


# 采样库的文件名五花八门：SalamanderGrandPianoV3_A3v16.wav / sus_A#3.wav / 060_C4_mf.wav …
# 先从整串解析，失败再从任意位置找"音名+八度"（取最后一个匹配，通常是真正的音高）。
_NOTE_RE = re.compile(r"(?<![A-Za-z])([A-Ga-g])([#sb]?)(-?\d{1,2})(?![0-9])")


def note_from_stem(stem: str) -> int | None:
    whole = note_from_name(stem)
    if whole is not None:
        return whole
    last = None
    for m in _NOTE_RE.finditer(stem):
        letter, acc, octave = m.group(1).upper(), m.group(2).lower(), int(m.group(3))
        if not -1 <= octave <= 9:
            continue
        # 注意：acc 可能是空串，而 Python 里 '' in "#s" 恒为 True —— 必须显式比较，
        # 否则每个自然音都会被 +1 个半音（C4 变 C#4），整库音高全错。
        if acc == "#" or acc == "s":
            step = NOTE_BASE[letter] + 1
        elif acc == "b":
            step = NOTE_BASE[letter] - 1
        else:
            step = NOTE_BASE[letter]
        midi = step + (octave + 1) * 12
        if 0 <= midi <= 127:
            last = midi
    return last


def read_wav(path: str) -> tuple[list[int], int]:
    """读成 (s16 单声道样本, 采样率)。8/16/24/32-bit PCM 与 float32 都认，立体声混合成单声道。"""
    with wave.open(path, "rb") as w:
        channels, width, rate, frames = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(frames)
    if width == 2:
        data = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
    elif width == 1:
        data = [(b - 128) << 8 for b in raw]
    elif width == 3:
        data = []
        for i in range(0, len(raw), 3):
            v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
            if v & 0x800000:
                v -= 0x1000000
            data.append(v >> 8)
    elif width == 4:
        data = [v >> 16 for v in struct.unpack("<%di" % (len(raw) // 4), raw)]
    else:
        raise SystemExit(f"{path}: 不支持的位宽 {width * 8} bit")
    if channels > 1:
        data = [sum(data[i:i + channels]) // channels for i in range(0, len(data) - channels + 1, channels)]
    return data, rate


@dataclass
class Zone:
    data: list[int]
    rate: int
    root: int
    low: int = 0
    high: int = 127
    loop: bool = False
    loop_start: int = 0
    loop_end: int = 0
    interp: int = 1          # 1 = 三次插值（库里的采样都是"少量精品"，插值值得）
    gain_q8: int = 256
    src: str = ""


@dataclass
class Instrument:
    name: str
    pc: int
    volume: int = 230
    mismatch_limit: int = 0  # 0 = 不限制键区
    zones: list[Zone] = field(default_factory=list)


def normalize(zone: Zone, peak: float) -> None:
    top = max((abs(v) for v in zone.data), default=0)
    if top == 0:
        return
    target = int(peak * 32767)
    g = target / top
    zone.gain_q8 = max(1, min(1023, int(round(g * 256))))
    if abs(g - 1.0) > 1e-3:
        zone.data = [max(-32768, min(32767, int(v * g))) for v in zone.data]


def auto_loop(zone: Zone) -> None:
    """在尾部找一段近似周期：取最后 25% 里与开头相关性最好的位置。"""
    n = len(zone.data)
    if n < 512:
        return
    start = int(n * 0.75)
    win = min(512, n - start)
    if win < 64:
        return
    ref = zone.data[start:start + win]
    best, best_pos = None, start
    step = max(1, win // 8)
    for pos in range(start, n - win, step):
        seg = zone.data[pos:pos + win]
        err = sum(abs(a - b) for a, b in zip(ref, seg))
        if best is None or err < best:
            best, best_pos = err, pos
    zone.loop = True
    zone.loop_start = best_pos
    zone.loop_end = n


def resample(data: list[int], src_rate: int, dst_rate: int) -> list[int]:
    """线性插值重采样。板子按 zone.rate 播放，降采样能直接省 QSPI 空间
    （48k→32k 省 1/3）。音高不变：根音是 zone.root，与采样率无关。"""
    if src_rate == dst_rate or not data:
        return data
    n = int(len(data) * dst_rate / src_rate)
    out = [0] * n
    ratio = src_rate / dst_rate
    last = len(data) - 1
    for i in range(n):
        pos = i * ratio
        i0 = int(pos)
        if i0 >= last:
            out[i] = data[last]
            continue
        frac = pos - i0
        a = data[i0]
        out[i] = int(a + (data[i0 + 1] - a) * frac)
    return out


def trim_zone(zone: Zone, max_seconds: float, fade_ms: float) -> None:
    """截断到 max_seconds，并在末尾加线性淡出（避免"啪"的一声）。0 = 不处理。"""
    if max_seconds > 0:
        want = int(max_seconds * zone.rate)
        if len(zone.data) > want:
            zone.data = zone.data[:want]
    if fade_ms > 0 and len(zone.data) > 8:
        n = min(len(zone.data), int(fade_ms * zone.rate / 1000.0))
        for i in range(n):
            g = (n - i) / n
            zone.data[len(zone.data) - n + i] = int(zone.data[len(zone.data) - n + i] * g)


def collect_instruments(src: str, manifest: dict, peak: float, verbose: bool,
                        max_seconds: float = 0.0, fade_ms: float = 0.0,
                        target_rate: int = 0) -> list[Instrument]:
    entries = manifest.get("instruments") if manifest else None
    if not entries:
        dirs = sorted(d for d in os.listdir(src) if os.path.isdir(os.path.join(src, d)))
        entries = [{"dir": d, "pc": 20 + i, "name": d.capitalize()} for i, d in enumerate(dirs)]

    instruments: list[Instrument] = []
    for entry in entries:
        sub = os.path.join(src, entry["dir"])
        if not os.path.isdir(sub):
            print(f"  ! 跳过 {entry['dir']}：目录不存在")
            continue
        inst = Instrument(
            name=str(entry.get("name", entry["dir"]))[:MAX_NAME],
            pc=int(entry.get("pc", 20 + len(instruments))),
            volume=int(entry.get("volume", 230)),
            mismatch_limit=int(entry.get("mismatch_limit", 0)),
        )
        roots = entry.get("roots", {})
        zones: list[Zone] = []
        by_root: dict[int, str] = {}
        for fname in sorted(os.listdir(sub)):
            if not fname.lower().endswith(".wav"):
                continue
            path = os.path.join(sub, fname)
            stem = os.path.splitext(fname)[0]
            root = roots.get(stem)
            if root is None:
                root = note_from_stem(stem)
            if root is None:
                root = int(entry.get("root", 60))
            root = int(root)
            if root in by_root:      # 同一个音有多个文件（多是力度层）→ 只留排序最靠前的那个
                if verbose:
                    print(f"    (跳过 {fname}：根音 {root} 已有 {by_root[root]})")
                continue
            data, rate = read_wav(path)
            if not data:
                print(f"  ! 跳过 {fname}：没有样本")
                continue
            if target_rate and target_rate != rate:
                data = resample(data, rate, target_rate)
                rate = target_rate
            by_root[root] = fname
            z = Zone(data=data, rate=rate, root=root, src=fname)
            trim_zone(z, max_seconds, fade_ms)
            normalize(z, peak)
            if entry.get("loop"):
                auto_loop(z)
            zones.append(z)
            if verbose:
                print(f"    {fname:24s} root={z.root:3d} frames={len(z.data):8d} rate={z.rate} loop={int(z.loop)}")
        if not zones:
            print(f"  ! 乐器 {inst.name} 一个 WAV 都没有，跳过")
            continue
        zones.sort(key=lambda z: z.root)
        for i, z in enumerate(zones):
            z.low = 0 if i == 0 else (zones[i - 1].root + zones[i].root + 1) // 2
            z.high = 127 if i == len(zones) - 1 else (z.root + zones[i + 1].root) // 2
        inst.zones = zones
        instruments.append(inst)
    return instruments


def build_library(instruments: list[Instrument]) -> bytes:
    zones = [z for inst in instruments for z in inst.zones]
    if len(zones) > 255:
        raise SystemExit(f"区太多（{len(zones)}），固件上限 255")

    desc = bytearray()
    # 乐器表在前、区表在后（固件按 header.instr_count / zone_count 顺序读）
    instr_bytes = bytearray()
    zone_bytes = bytearray()
    data = bytearray()
    first = 0
    for inst in instruments:
        instr_bytes += struct.pack(
            INSTR_FMT,
            inst.name.encode("ascii", "replace")[:MAX_NAME].ljust(16, b"\0"),
            inst.pc & 0xFF, len(inst.zones) & 0xFF, inst.volume & 0xFF,
            inst.mismatch_limit & 0xFF, first, 0,
        )
        for z in inst.zones:
            off = DATA_BASE + len(data)      # 固件按"库内绝对偏移"用：0x90000000 + offset
            data += struct.pack("<%dh" % len(z.data), *z.data)
            while len(data) % 4:
                data += b"\0"
            zone_bytes += struct.pack(
                ZONE_FMT,
                off, len(z.data), z.rate, z.loop_start, z.loop_end,
                z.root & 0xFF, z.low & 0xFF, z.high & 0xFF, 1 if z.loop else 0,
                z.interp & 0xFF, 0, 0, 0, z.gain_q8 & 0xFFFF, 0,
            )
        first += len(inst.zones)

    desc += instr_bytes + zone_bytes
    if DESC_BYTES + len(data) > DATA_LIMIT:
        raise SystemExit(
            f"库太大：数据 {len(data) / 1048576:.2f} MB，最多 {(DATA_LIMIT - DESC_BYTES) / 1048576:.2f} MB"
        )

    serial = zlib.crc32(bytes(desc) + bytes(data)) & 0xFFFFFFFF
    header = struct.pack(
        HDR_FMT, MAGIC, VERSION, HDR_BYTES, len(instruments), len(zones),
        DATA_BASE, len(data), serial, 0, b"\0\0\0\0",
    )
    blob = bytearray(header)
    blob += desc
    blob += b"\0" * (DATA_BASE - len(blob))
    blob += data
    return bytes(blob), serial, len(zones)


def selftest_library() -> tuple[bytes, int, int]:
    """固件自检用的同款小库：PC 120，2048 帧 937.5 Hz-ish 正弦（这里用 1000 Hz 听得出区别）。"""
    import math
    frames = 2048
    data = [int(0.6 * 32767 * math.sin(2 * math.pi * 1000 * i / 48000)) for i in range(frames)]
    z = Zone(data=data, rate=48000, root=69, loop=True, loop_start=0, loop_end=frames, src="selftest")
    inst = Instrument(name="SelfTest", pc=120, volume=255, zones=[z])
    return build_library([inst])


def list_library(path: str) -> None:
    blob = open(path, "rb").read()
    magic, version, hdr, ninst, nzone, doff, dbytes, serial, crc, _ = struct.unpack_from(HDR_FMT, blob, 0)
    print(f"{path}: magic=0x{magic:X} version={version} instr={ninst} zone={nzone} "
          f"data={dbytes / 1048576:.2f} MB serial=0x{serial:08X}")
    instr_sz = struct.calcsize(INSTR_FMT)
    zone_sz = struct.calcsize(ZONE_FMT)
    zone_base = hdr + ninst * instr_sz          # 区表在所有乐器条目之后（固件 lib_zones() 同款算法）
    for i in range(ninst):
        name, pc, zc, vol, mml, first, _r = struct.unpack_from(INSTR_FMT, blob, hdr + i * instr_sz)
        nm = name.split(b"\0")[0].decode("ascii", "replace")
        print(f"  PC {pc:3d}  {nm:16s} zones={zc} volume={vol} mismatch_limit={mml}")
        for k in range(zc):
            off, frames, rate, ls, le, root, low, high, flags, interp, *_ = struct.unpack_from(
                ZONE_FMT, blob, zone_base + (first + k) * zone_sz)
            print(f"      zone offset=0x{off:06X} frames={frames:7d} rate={rate:6d} root={root:3d} "
                  f"[{low:3d}..{high:3d}] loop={flags & 1} ls={ls} le={le} interp={interp}")


def main() -> int:
    ap = argparse.ArgumentParser(description="打包 QSPI 采样库 samples.bin")
    ap.add_argument("--src", default="samples", help="源目录（默认 samples/）")
    ap.add_argument("--out", default="samples.bin", help="输出文件（默认 samples.bin）")
    ap.add_argument("--peak", type=float, default=0.9, help="每个采样的归一化峰值（默认 0.9）")
    ap.add_argument("--selftest", action="store_true", help="生成固件自检用的同款小库")
    ap.add_argument("--list", metavar="BIN", help="列出已有库的内容")
    ap.add_argument("--max-seconds", type=float, default=0.0, help="每个采样最多保留几秒（0 = 不截断）")
    ap.add_argument("--fade-out-ms", type=float, default=0.0, help="截断时的淡出时长 ms（建议 30）")
    ap.add_argument("--rate", type=int, default=0, help="重采样到该采样率（如 32000；0 = 保持原样，48k→32k 省 1/3 空间）")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.list:
        list_library(args.list)
        return 0

    if args.selftest:
        blob, serial, nzone = selftest_library()
        open(args.out, "wb").write(blob)
        print(f"自检库已写到 {args.out}：1 件乐器（PC 120 SelfTest）、{nzone} 个区、"
              f"{len(blob) / 1024:.1f} KB、serial=0x{serial:08X}")
        print("拷到 SD 卡 /synth/samples.bin → 上电（或删掉库后重启）→ 发 PC 120 试听")
        return 0

    if not os.path.isdir(args.src):
        print(f"源目录 {args.src} 不存在。用 --selftest 先验证通路，或按 README 放好 WAV。")
        return 1
    manifest = {}
    mpath = os.path.join(args.src, "manifest.json")
    if os.path.isfile(mpath):
        manifest = json.load(open(mpath, "r", encoding="utf-8"))
        print(f"读入 {mpath}")

    instruments = collect_instruments(args.src, manifest, args.peak, args.verbose,
                                      args.max_seconds, args.fade_out_ms, args.rate)
    if not instruments:
        print("没有可打包的乐器")
        return 1

    blob, serial, nzone = build_library(instruments)
    open(args.out, "wb").write(blob)
    used = len(blob) / 1048576
    print(f"\n{args.out}: {len(instruments)} 件乐器 / {nzone} 个区 / {used:.2f} MB / serial=0x{serial:08X}")
    for inst in instruments:
        mb = sum(len(z.data) for z in inst.zones) * 2 / 1048576
        print(f"  PC {inst.pc:3d}  {inst.name:16s} {len(inst.zones):2d} 区  {mb:.2f} MB")
    print(f"QSPI 8 MB 里还剩 {QSPI_SIZE / 1048576 - used:.2f} MB（其中最后一个 4 KB 扇区不能用）")
    print("下一步：把 samples.bin 拷到 SD 卡 /synth/ 下 → 板子上电（会提示导入中）→ 发对应 PC 试听")
    return 0


if __name__ == "__main__":
    sys.exit(main())
