#!/usr/bin/env python3
"""pick_samples.py —— 从下载来的大采样库里挑出我们要的那几个音。

采样库动辄几百 MB / 几千个文件（VSCO 2 CE 3 GB、Salamander 700 MB），但板子只有
8 MB QSPI，所以只挑：**每 N 个半音一个音、一个力度层、一个 round-robin**。

    # 钢琴：UprightPianoKW（A0vH.wav / A0vL.wav …），每 4 个半音一个，取轻力度层
    python tools/pick_samples.py --lib samples/UprightPianoKW-SFZ-20220221/samples \
        --match "^[A-G][#sb]?-?[0-9]+v[HL]$" --prefer "vL$" --every 4 --out samples/piano

    # 弦乐：VSCO 小提琴组的持续音（VlnEns_susVib_A2_v1_rr1.wav）
    python tools/pick_samples.py --lib "samples/VSCO-2-CE-1.1.0/Strings/Violin Section" \
        --match "VlnEns_susVib" --prefer "_v1_rr1" --every 5 --out samples/strings

挑出来的文件会改名成 `<音名>.wav`（如 A2.wav / Ds4.wav）放进 --out，随后：
    python tools/pack_samples.py --max-seconds 2.5 --fade-out-ms 40 --rate 32000
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pack_samples import note_from_stem  # noqa: E402  （复用同一套文件名音高解析）

NAMES = ["C", "Cs", "D", "Ds", "E", "F", "Fs", "G", "Gs", "A", "As", "B"]


def note_name(midi: int) -> str:
    return f"{NAMES[midi % 12]}{midi // 12 - 1}"


def main() -> int:
    ap = argparse.ArgumentParser(description="从大采样库里挑出要用的音")
    ap.add_argument("--lib", required=True, help="采样库目录（递归找 WAV）")
    ap.add_argument("--out", required=True, help="输出目录（会创建）")
    ap.add_argument("--match", default="", help="文件名必须匹配的正则（不区分大小写）")
    ap.add_argument("--prefer", default=r"_v1_|_v1$|rr1|vL$", help="同一个音有多个文件时优先选谁")
    ap.add_argument("--every", type=int, default=4, help="每隔几个半音留一个（默认 4）")
    ap.add_argument("--max", type=int, default=16, help="最多留几个音（默认 16，按音高均匀抽稀）")
    ap.add_argument("--ext", default=".wav", help="源文件扩展名（默认 .wav，也可 .flac 等）")
    ap.add_argument("--dry-run", action="store_true", help="只打印挑了什么，不复制")
    args = ap.parse_args()

    if not os.path.isdir(args.lib):
        print(f"目录不存在：{args.lib}")
        return 1
    match = re.compile(args.match, re.IGNORECASE) if args.match else None
    prefer = re.compile(args.prefer, re.IGNORECASE) if args.prefer else None

    by_note: dict[int, list[str]] = {}
    for root, _dirs, files in os.walk(args.lib):
        for f in files:
            if not f.lower().endswith(args.ext.lower()):
                continue
            stem = os.path.splitext(f)[0]
            if match and not match.search(stem):   # 只匹配文件名主体（不含扩展名）
                continue
            n = note_from_stem(stem)
            if n is None:
                continue
            by_note.setdefault(n, []).append(os.path.join(root, f))
    if not by_note:
        print("没找到匹配的采样（检查 --lib / --match / --ext）")
        return 1

    # 每个音只留一个文件：优先 prefer 命中的，其次名字短的（少 rr 后缀）
    picked: dict[int, str] = {}
    for n, files in by_note.items():
        files.sort(key=lambda p: (0 if (prefer and prefer.search(os.path.basename(p))) else 1,
                                  len(os.path.basename(p)), os.path.basename(p)))
        picked[n] = files[0]

    notes = sorted(picked)
    step = max(1, args.every)
    keep = notes[::step]
    # 均匀抽稀到 --max 个（保留最低与最高音，键盘两端才不会被拉伸得太狠）
    while len(keep) > args.max:
        drop_every = max(2, len(keep) // (len(keep) - args.max + 1))
        keep = [n for i, n in enumerate(keep) if i == 0 or i == len(keep) - 1 or i % drop_every]

    os.makedirs(args.out, exist_ok=True)
    print(f"库里匹配到 {len(notes)} 个音，挑出 {len(keep)} 个 → {args.out}")
    total = 0
    for n in keep:
        src = picked[n]
        dst = os.path.join(args.out, note_name(n) + os.path.splitext(src)[1])
        size = os.path.getsize(src)
        total += size
        print(f"  {note_name(n):5s} (MIDI {n:3d})  {size / 1024:8.1f} KB  {os.path.basename(src)}")
        if not args.dry_run:
            shutil.copyfile(src, dst)
    print(f"源文件合计 {total / 1048576:.1f} MB（打包后会小很多：单声道 + 截断 + 重采样）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
