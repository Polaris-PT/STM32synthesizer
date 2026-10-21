# QSPI 采样库（samples.bin）—— 格式、打包、上板流程

板载 8 MB QSPI flash（W25Q64，映射在 `0x90000000`）用来放**采样乐器**：一台钢琴、
一把弦乐、一把贝斯、一套打击乐这类"少量精品"。SD 卡是**导入通道**（大文件、可随时换），
QSPI 是**运行时存储**（映射读，零拷贝、不受 SD 延迟影响）。

## 为什么不是"直接放 SD 卡"

| | SD 卡（FatFs） | QSPI（内存映射） |
|---|---|---|
| 访问方式 | 块设备 + 文件系统，单次 `f_read` 0.1–1 ms | 直接按指针读，四线 48 MHz ≈ 20 MB/s |
| 音频线程能不能用 | **不能**（SDMMC 是 DMA/IDMA + 文件系统，会阻塞、会超时，实测能把主循环卡死 30 s） | 能，和读内部 RAM 一样（只慢一点） |
| 换内容 | 拔卡拷文件 | 掉电重写（几 MB 要 1–2 分钟） |

结论：**SD 进、QSPI 出**。开机时如果卡上 `/synth/samples.bin` 的 `serial` 与板上库不同，
固件会自动把它导入 QSPI（`synth_port_lib_import_from_card()`，导入期间板子看起来像卡住，
几 MB 大约 1–2 分钟）。

## 库文件格式（version 2，小端）

```
偏移        内容
0x0000      头 36 B：magic 'SLIB'(0x42494C53) | version(u16)=2 | hdr_bytes(u16)=36
            instr_count(u32) | zone_count(u32) | data_offset(u32)=0x4000
            data_bytes(u32) | serial(u32) | crc(u32，保留) | reserved[4]
0x0024      乐器表 instr_count × 24 B：
            name[16] | pc(u8) | zone_count(u8) | volume(u8) | mismatch_limit(u8)
            | first_zone(u16) | reserved2(u16)
0x0024+24N 区表 zone_count × 32 B：
            offset(u32，**库内绝对偏移**，即 0x90000000+offset) | frames(u32) | rate(u32)
            | loop_start(u32) | loop_end(u32) | root(u8) | low(u8) | high(u8)
            | flags(u8，bit0 = 循环) | interp(u8，0 线性 / 1 三次) | pad[3]
            | gain_q8(u16) | crossfade(u16)
0x4000      采样数据：s16 单声道，逐区紧密排列、4 字节对齐
0x7FF000    上限（最后一个 4 KB 扇区留给 QSPI 读写自检，库不要用）
```

- `serial`：打包时按内容算的 CRC32。固件用它判断"卡上的库是不是新的"。
- `mismatch_limit`：单个采样最多管多少个半音（0 = 不限制）。打包器按相邻采样根音的
  中点自动算键区，`mismatch_limit` 用来防止"一个采样被拉去弹两个八度"。
- 固件侧定义在 `Synth/port/sample_storage_qspi.c`（改格式必须两边一起改，并把 `VERSION` +1）。

## 打包

```powershell
# 1) 放素材：samples/<乐器目录>/*.wav，文件名用根音（C4.wav / Ds3.wav / A#2.wav / 60.wav）
# 2) 打包
python tools\pack_samples.py                 # → samples.bin
python tools\pack_samples.py --list samples.bin
python tools\pack_samples.py --selftest      # 只造一个 1 kHz 小库，验证整条通路
```

`manifest.json`（可选）指定 PC 号、名字、音量、循环等：

```json
{ "instruments": [
  { "dir": "piano",  "pc": 20, "name": "Piano",   "volume": 230 },
  { "dir": "strings","pc": 21, "name": "Strings", "volume": 200, "loop": true },
  { "dir": "perc",   "pc": 30, "name": "Perc",    "roots": {"kick": 36, "snare": 38} }
] }
```

缺省时目录名当乐器名、PC 从 20 起依次分配、根音从文件名解析、不循环。

## 上板流程

1. `samples.bin` 拷到 SD 卡 `/synth/` 目录（卡先在本机格式化成 FAT32）。
2. 插卡、上电。固件在启动时挂载卡 → 读 `samples.bin` 头 → `serial` 与板上库不同就导入
   QSPI（这段期间板子像卡住，1–2 分钟；导入状态可以用 `tools\status.ps1` 看 `lib_import`：
   -1 = 正在导入，1 = 已是最新/导入成功，-1..-5 = 失败原因）。
3. 导入完成后正常进入主循环。用 `tools\status.ps1` 看 `lib_count`（乐器数）和 `lib_serial`。
4. 发对应 PC 试听：`lib_has_pc(pc)` 命中就直接挂载播放（零拷贝），
   否则再回落 SD 卡的 `/synth/NNN.ins`、`/synth/NNN.syn`，最后才是内置 15 个音色。

## 相关文件

| 文件 | 职责 |
|---|---|
| `tools/pack_samples.py` | PC 端打包器（本文件描述的就是它产出的格式） |
| `Synth/port/qspi_flash.c/.h` | QSPI 底层：擦扇区 / 页编程 / 间接读 / 进出内存映射 |
| `Synth/port/sample_storage_qspi.c` | 库：校验、枚举、按 PC 零拷贝挂载、从卡导入、自检 |
| `Synth/port/sample_storage_sd.c` | 采样池（RAM）+ WAV 加载（128 KB 上限） |
| `Synth/port/patch_storage_sd.c` | 音色映射持久化（`/synth/state.cfg`）+ 按 PC 找卡上音色 |

## 已验证 / 未验证

已上板验证：
- QSPI 数据通路：写 16 B 图案到 `0x7FF000` 扇区 → 从映射窗口读回一致（`qspi_wtest = 0`）。
- 导入 + 枚举 + 零拷贝挂载：固件自检库（PC 120）导入 QSPI 后
  `lib_count = 1`、`g_qspi_instrument` 的 zone 指向 `0x90004000`、`frames = 2048`。
- 格式一致性：打包器能逐字段解析固件写出的库（magic/version/表偏移/zone 字段全对）。

待验证：
- 真采样（钢琴/弦乐/贝斯/打击乐）打包后的听感与音量平衡。
- 从 SD 卡导入真库（几 MB）的耗时与中断期间表现。

## 四件套采样库（已挑好、已打包，2026-10-19）

从下载的三个大库里挑出来（`tools/pick_samples.py`），再打包（`tools/pack_samples.py --max-seconds 3.0 --fade-out-ms 60 --rate 32000`）：

| 目录 | PC | 音源 | 挑法 | 结果 |
|---|---|---|---|---|
| `samples/piano` | 20 | UprightPianoKW-SFZ（Kawai 立式，66 个 WAV） | 每 3 个半音一个音、取硬力度层 `vH` | 13 区 1.89 MB |
| `samples/strings` | 21 | VSCO 2 CE `Strings/Violin Section` → `VlnEns_susVib`（颤音持续音） | 每 3 个半音、`_v1_rr1`，循环播放 | 6 区 1.10 MB |
| `samples/bass` | 22 | VSCO 2 CE `Strings/Solo Contrabass` → `BKCtbss_SusNV`（低音提琴持续音） | 每 3 个半音、`_v1_rr1`，循环播放 | 7 区 1.28 MB |
| `samples/perc` | 23 | VSCO 2 CE `Percussion` 单发音 | 手工配 GM 键位 36/38/39/42/44/45/46/48/49/51 | 10 区 0.96 MB |

- 重采样到 **32 kHz**（`--rate 32000`，线性插值）；每个采样最多 3 秒 + 60 ms 淡出；单声道；峰值归一化 0.9。
- 打包结果：**`samples.bin` = 5.25 MB，4 件乐器 / 36 个区，serial = 0x7400D553**，QSPI 8 MB 里还剩 2.75 MB（最后一个 4 KB 扇区 `0x7FF000` 留给 QSPI 自检）。
- 打击乐键位：36 大鼓 / 38 军鼓 / 39 响板 / 42 沙锤(闭镲位) / 44 牛铃 / 45 低桶鼓 / 46 铃鼓(开镲位) / 48 高桶鼓 / 49 吊镲 / 51 叮叮镲；`mismatch_limit = 2`，所以只有这几个键附近会响。
- 许可：UprightPianoKW 与 VSCO 2 CE 都可自由使用（VSCO 2 CE 是 CC0），仅供自己玩，不要再分发 `samples/` 或 `samples.bin`（已加进 `.gitignore`）。

上板：把 `samples.bin` 拷到 SD 卡的 `/synth/` 目录 → 插卡上电 → 固件发现 serial 不同就自动导入（5.25 MB 的擦写约几十秒，导入完成后 USB 才枚举）→ 之后发 PC 20..23 即可试听。
