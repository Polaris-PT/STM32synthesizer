#!/usr/bin/env python3
"""把 RAM overlay 模块的 ELF 打包成板子认的 ui.bin（头 + 裸镜像）。

头布局与固件 `Synth/port/module_api.h` 的 synth_module_hdr_t 完全一致：
    magic(u32) version(u16) hdr_bytes(u16) abi(u32) build_id(u32)
    image_bytes(u32) entry_offset(u32) crc32(u32) reserved[2](u32)   => 36 字节

镜像 = objcopy -O binary 的产物，再补零到「.text/.rodata/.data/.bss 的最高地址 − 基址」，
这样 .bss 的零也在镜像里（模块没有 C 运行时，靠宿主 memcpy 一次性铺好）。

用法：
    python tools\\make_module.py --elf build\\ui_module.elf --out build\\ui.bin
产物拷到 SD 卡的 /synth/ui.bin，板子上电（或按复位）会自动导入 QSPI。
"""
import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

MAGIC = 0x444F4D53          # 'SMOD'
VERSION = 1
ABI = 1
# magic, version, hdr_bytes, abi, build_id, image_bytes, entry_offset, crc32, reserved[2]
HDR_FMT = "<IHHIIIIIII"
HDR_BYTES = struct.calcsize(HDR_FMT)     # 36
QSPI_MODULE_MAX = 0xF000                 # 60 KB（与 module_api.h 一致）
SKIP_SECTIONS = (".debug", ".comment", ".ARM.attributes", ".ARM.exidx", ".ARM.extab")


def run(cmd):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit("[fail] " + " ".join(str(c) for c in cmd) + "\n" + p.stdout + p.stderr)
    return p.stdout


def image_bytes_from_objdump(text, base):
    """取 .text/.rodata/.data/.bss 的最高结束地址，算镜像长度（含 .bss 的零）。"""
    end = base
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 4 or not parts[0].isdigit():
            continue
        name = parts[1]
        if name.startswith(SKIP_SECTIONS):
            continue
        try:
            size = int(parts[2], 16)
            vma = int(parts[3], 16)
        except ValueError:
            continue
        if size and vma >= base:
            end = max(end, vma + size)
    return end - base


def main():
    ap = argparse.ArgumentParser(description="打包 RAM overlay 模块（ui.bin）")
    ap.add_argument("--elf", required=True, help="模块 ELF（CMake 的 ui_module 目标产物）")
    ap.add_argument("--out", required=True, help="输出路径，例如 build\\ui.bin")
    ap.add_argument("--objcopy", default="arm-none-eabi-objcopy")
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--base", default="0x24070000", help="模块执行基址（链接脚本 ORIGIN）")
    ap.add_argument("--entry", default="ui_entry", help="入口符号名")
    args = ap.parse_args()

    base = int(args.base, 16)
    elf = Path(args.elf)
    if not elf.is_file():
        sys.exit(f"[fail] 找不到 {elf}")

    tmp = elf.with_suffix(".rawbin")
    run([args.objcopy, "-O", "binary", "--gap-fill", "0x00", elf, tmp])
    raw = tmp.read_bytes()
    tmp.unlink(missing_ok=True)

    need = image_bytes_from_objdump(run([args.objdump, "-h", elf]), base)
    image_bytes = (max(need, len(raw)) + 3) & ~3
    img = raw + b"\x00" * (image_bytes - len(raw))
    if HDR_BYTES + image_bytes > QSPI_MODULE_MAX:
        sys.exit(f"[fail] 镜像 {image_bytes} B + 头 {HDR_BYTES} B 超过模块区 {QSPI_MODULE_MAX} B")

    entry_addr = None
    for line in run([args.nm, elf]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == args.entry:
            entry_addr = int(parts[0], 16)
            break
    if entry_addr is None:
        sys.exit(f"[fail] 符号表里没有入口 {args.entry}")
    entry_off = entry_addr - base
    if entry_off < 0 or entry_off + 4 > image_bytes:
        sys.exit(f"[fail] 入口偏移 0x{entry_off:X} 不在镜像范围内")

    crc = zlib.crc32(img) & 0xFFFFFFFF
    hdr = struct.pack(HDR_FMT, MAGIC, VERSION, HDR_BYTES, ABI, crc, image_bytes,
                      entry_off, crc, 0, 0)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(hdr + img)
    print(f"[ok] {out}  头 {HDR_BYTES} B + 镜像 {image_bytes} B = {HDR_BYTES + image_bytes} B"
          f"  crc=0x{crc:08X}  entry=+0x{entry_off:X}  build_id=0x{crc:08X}")
    print(f"     拷到 SD 卡 /synth/ui.bin，板子上电即自动导入")


if __name__ == "__main__":
    main()
