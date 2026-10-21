#!/usr/bin/env python3
"""把 RAM overlay 模块镜像（ui.bin）通过 USB-MIDI SysEx 灌进板子 —— 不用拔卡、不用烧写。

协议（固件侧 Synth/port/module_upload.c）：
  F0 7D 00 <len:8 hex> F7                  开始（擦扇区）
  F0 7D 01 <off:6 hex> <data hex...> F7    数据块
  F0 7D 02 F7                              提交（校验头 + CRC 后重新加载模块）

用法：
  python tools\\upload_module.py                     # 上传 build\\RelWithDebInfo\\ui.bin
  python tools\\upload_module.py --file path\\ui.bin
  python tools\\upload_module.py --list             # 列出 MIDI 输出设备
  python tools\\upload_module.py --device 1 --chunk 200 --verbose

注意：上传期间主循环会停（QSPI 擦写是轮询的），声音会断约 1 秒，属正常。
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

MHDR_DONE = 0x00000001
CALLBACK_NULL = 0x00000000


class MIDIHDR(ctypes.Structure):
    _fields_ = [
        ("lpData", ctypes.c_void_p),
        ("dwBufferLength", wt.DWORD),
        ("dwBytesRecorded", wt.DWORD),
        ("dwUser", ctypes.c_size_t),
        ("dwFlags", wt.DWORD),
        ("lpNext", ctypes.c_void_p),
        ("reserved", ctypes.c_size_t),
        ("dwOffset", wt.DWORD),
        ("dwReserved", ctypes.c_size_t * 8),
    ]


class MIDIOUTCAPS(ctypes.Structure):
    _fields_ = [
        ("wMid", wt.WORD),
        ("wPid", wt.WORD),
        ("vDriverVersion", wt.UINT),
        ("szPname", wt.WCHAR * 32),
        ("wTechnology", wt.WORD),
        ("wVoices", wt.WORD),
        ("wNotes", wt.WORD),
        ("wChannelMask", wt.WORD),
        ("dwSupport", wt.DWORD),
    ]


winmm = ctypes.WinDLL("winmm")
winmm.midiOutGetNumDevs.restype = wt.UINT
winmm.midiOutGetDevCapsW.argtypes = [ctypes.c_size_t, ctypes.POINTER(MIDIOUTCAPS), wt.UINT]
winmm.midiOutOpen.argtypes = [ctypes.POINTER(ctypes.c_void_p), wt.UINT, ctypes.c_size_t, ctypes.c_size_t, wt.DWORD]
winmm.midiOutClose.argtypes = [ctypes.c_void_p]
winmm.midiOutPrepareHeader.argtypes = [ctypes.c_void_p, ctypes.POINTER(MIDIHDR), wt.UINT]
winmm.midiOutUnprepareHeader.argtypes = [ctypes.c_void_p, ctypes.POINTER(MIDIHDR), wt.UINT]
winmm.midiOutLongMsg.argtypes = [ctypes.c_void_p, ctypes.POINTER(MIDIHDR), wt.UINT]


def list_devices():
    n = winmm.midiOutGetNumDevs()
    out = []
    for i in range(n):
        caps = MIDIOUTCAPS()
        if winmm.midiOutGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
            out.append((i, caps.szPname))
    return out


def find_device(name_hint, index):
    devs = list_devices()
    if index is not None:
        for i, nm in devs:
            if i == index:
                return i, nm
        raise SystemExit(f"找不到 MIDI 输出设备索引 {index}；现有：{devs}")
    for i, nm in devs:
        if name_hint.lower() in nm.lower():
            return i, nm
    raise SystemExit(f"没找到名字含 '{name_hint}' 的 MIDI 输出设备；现有：{devs}")


class Sender:
    def __init__(self, device_index):
        self.h = ctypes.c_void_p()
        rc = winmm.midiOutOpen(ctypes.byref(self.h), device_index, 0, 0, CALLBACK_NULL)
        if rc != 0:
            raise SystemExit(f"midiOutOpen 失败，rc={rc}")

    def close(self):
        if self.h:
            winmm.midiOutClose(self.h)
            self.h = ctypes.c_void_p()

    def send(self, data: bytes, pace=0.003, timeout=2.0):
        buf = ctypes.create_string_buffer(data, len(data))
        hdr = MIDIHDR()
        hdr.lpData = ctypes.cast(buf, ctypes.c_void_p)
        hdr.dwBufferLength = len(data)
        if winmm.midiOutPrepareHeader(self.h, ctypes.byref(hdr), ctypes.sizeof(hdr)) != 0:
            raise SystemExit("midiOutPrepareHeader 失败")
        rc = winmm.midiOutLongMsg(self.h, ctypes.byref(hdr), ctypes.sizeof(hdr))
        if rc != 0:
            winmm.midiOutUnprepareHeader(self.h, ctypes.byref(hdr), ctypes.sizeof(hdr))
            raise SystemExit(f"midiOutLongMsg 失败，rc={rc}（消息 {len(data)} 字节）")
        t0 = time.time()
        while not (hdr.dwFlags & MHDR_DONE):
            if time.time() - t0 > timeout:
                break
            time.sleep(0.001)
        winmm.midiOutUnprepareHeader(self.h, ctypes.byref(hdr), ctypes.sizeof(hdr))
        if pace:
            time.sleep(pace)


def build_messages(blob: bytes, chunk: int):
    yield bytes([0xF0, 0x7D, 0x00]) + f"{len(blob):08X}".encode("ascii") + bytes([0xF7])
    off = 0
    while off < len(blob):
        piece = blob[off:off + chunk]
        yield (bytes([0xF0, 0x7D, 0x01]) + f"{off:06X}".encode("ascii") +
               piece.hex().upper().encode("ascii") + bytes([0xF7]))
        off += len(piece)
    yield bytes([0xF0, 0x7D, 0x02, 0xF7])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default=os.path.join("build", "RelWithDebInfo", "ui.bin"))
    ap.add_argument("--device", type=int, default=None, help="MIDI 输出设备索引（默认按名字找）")
    ap.add_argument("--name", default="H750 PCM5102A Synth", help="设备名关键字")
    ap.add_argument("--chunk", type=int, default=240, help="每个 SysEx 携带的镜像字节数")
    ap.add_argument("--pace", type=float, default=0.003, help="每条消息之间的间隔（秒）")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if args.list:
        for i, nm in list_devices():
            print(f"index {i} : {nm}")
        return 0

    if not os.path.isfile(args.file):
        raise SystemExit(f"文件不存在：{args.file}")
    blob = open(args.file, "rb").read()
    if len(blob) < 40:
        raise SystemExit("文件太小，不像 ui.bin")
    import struct
    magic, version, hdr_bytes, abi, build_id, image_bytes, entry, crc = struct.unpack_from("<I H H I I I I I", blob, 0)
    if magic != 0x444F4D53:
        raise SystemExit(f"头部 magic 不对：0x{magic:08X}")
    # 在头部 reserved[0] 打上 "USB" 标记（偏移 32）：固件看到它就不再拿卡上的旧
    # ui.bin 覆盖板上这份（否则每次上电会被卡里的旧镜像顶回去）。
    blob = bytearray(blob)
    struct.pack_into("<I", blob, 32, 0x55534255)
    blob = bytes(blob)
    print(f"[info] {args.file}: {len(blob):,} B  (头 {hdr_bytes} B + 镜像 {image_bytes:,} B, "
          f"version {version}, abi {abi}, crc 0x{crc:08X}, entry +0x{entry:X})")

    idx, nm = find_device(args.name, args.device)
    print(f"[info] MIDI 输出设备 {idx}: {nm}")
    sender = Sender(idx)
    try:
        msgs = list(build_messages(blob, args.chunk))
        print(f"[info] 共 {len(msgs)} 条消息（开始 1 + 数据 {len(msgs) - 2} + 提交 1）")
        t0 = time.time()
        for k, m in enumerate(msgs):
            sender.send(m, pace=args.pace)
            if args.verbose and (k % 8 == 0 or k == len(msgs) - 1):
                print(f"  [{k + 1}/{len(msgs)}] {len(m)} B")
        print(f"[ok] 上传完成，用时 {time.time() - t0:.2f} s")
        print("[hint] 看屏幕；或跑 tools\\status.ps1 看 module_state / module_crc / module_ticks")
    finally:
        sender.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
