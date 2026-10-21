# RAM overlay 模块（TFT UI 等可选功能的存放与执行方式）

> 目标：H750 只有 **128 KB 内部 flash**，而 TFT UI（ST7735 驱动 + 字体 + 页面逻辑）大概要
> 10–20 KB。本机制把这类"可选功能"编成**独立模块**存在外部 QSPI flash 里，上电后拷到
> AXI SRAM 执行 —— **不占内部 flash，也不影响音频热路径**。

## 1. 为什么是"拷到 RAM 执行"而不是"直接在 QSPI 里执行"

| 方案 | 做法 | 问题 |
|---|---|---|
| XIP（就地执行） | 把代码段链到 `0x90000000` 直接跑 | 每次 I-Cache 未命中要等 QSPI（32 B/1.3 µs），热路径不可用；还得把 MPU 的 `0x90000000` 区从"不可执行"改成可执行；软断点失效 |
| **RAM overlay（本方案）** | QSPI 存镜像 → 上电校验 → `memcpy` 到 AXI SRAM → 调用 | 需要 64 KB RAM（AXI 512 KB 里划出）与一次拷贝（几十 KB，几毫秒） |
| 纯 SD 加载 | 每次启动从卡读 | 拔卡就没 UI；启动依赖卡 |

AXI SRAM 目前只用了约 78%，划出顶部 64 KB（`0x24070000`）给模块完全没有压力。

## 2. 布局

```
QSPI 8 MB
  0x000000  采样库（samples.bin 导入，最大 ~7.9 MB）
  0x7F0000  ┌──────────────────────────────┐
            │ synth_module_hdr_t (36 B)    │  magic 'SMOD' / version / abi / build_id /
            ├──────────────────────────────┤  image_bytes / entry_offset / crc32
            │ 裸镜像（.text/.rodata/.data/ │
            │  .bss 的零也在里面）         │
            └──────────────────────────────┘
  0x7FF000  保留（QSPI 写自检扇区）

AXI SRAM
  0x24000000 … 0x2406FFFF  宿主 .data/.bss/.ram_section（链接脚本 RAM = 448 KB）
  0x24070000 … 0x2407FFFF  模块执行区（链接脚本 MODULE_RAM = 64 KB）
```

`STM32H750xx_FLASH.ld` 里 `RAM` 故意缩成 448 KB：宿主的静态数据要是超了，**链接直接失败**，
不会悄悄侵占模块区（这是护栏，不是巧合）。

## 3. 宿主 ↔ 模块的契约（`Synth/port/module_api.h`）

模块没有符号表、没有 C 运行时（`-nostdlib`：没有 startup、没有 `memcpy/memset/printf`），
所以两边只通过**函数指针表**通信：

- 宿主传给模块 `synth_module_api_t`：`get_tick` / `get_status` / `set_patch` /
  `set_channel_patch` / `lib_info` / `sd_present` / `log` / `set_probe`
- 模块返回 `synth_module_cbs_t`：`tick(now_ms)`（宿主 100 Hz 调用）、`key(pressed)`（PC13 按键）
- 入口：`const synth_module_cbs_t *ui_entry(const synth_module_api_t *api)`
- `synth_module_status_t` 是一次性快照（CPU 占用、声部数、音色号、库信息…），UI 只读它

**ABI 号**（`SYNTH_MODULE_ABI`）任何一侧改了结构体就必须 +1：宿主校验 ABI，对不上拒绝加载
（`module_state = -2`），绝不会带着错位的函数指针跑飞。

## 4. 构建与部署

```powershell
# 1) 编固件 + 模块（CMake 的 ui_module 目标会自动跑，产物在 build\RelWithDebInfo\）
tools\build.ps1 -Generator 'MinGW Makefiles'

# 2) 把模块放到卡上（工具链自带的 SD 卡读卡器，或用板子 USB 的 MSC——目前没做）
copy build\RelWithDebInfo\ui.bin  <SD 卡>:\synth\ui.bin

# 3) 插卡上电：固件自动把 /synth/ui.bin 写进 QSPI 的 0x7F0000（build_id/CRC 相同则跳过）
tools\build.ps1 -Flash
tools\status.ps1        # 看 module_import / module_state / module_ticks / module_probe
```

`tools/make_module.py` 负责打包：`arm-none-eabi-objcopy -O binary` → 按 `.text/.rodata/.data/.bss`
的最高地址补零（这样 `.bss` 的零也在镜像里）→ 从符号表取 `ui_entry` 偏移 → 算 CRC32 →
拼 36 字节头。

## 5. 返回码（`g_app_status.module_state`，`tools\status.ps1` 已解码）

| 值 | 含义 |
|---|---|
| `0` | 已加载，模块在 tick 循环里跑 |
| `-1` | QSPI 里没有模块（魔数不对 → 还没导入 `ui.bin`） |
| `-2` | 版本/ABI 不匹配 → 重新打包 `ui.bin` |
| `-3` | CRC 校验失败（镜像损坏/写一半断电） |
| `-4` | 长度或入口偏移非法 |
| `-5` | `ui_entry` 返回 NULL（模块自己拒绝初始化） |

`module_import` 是导入结果：`0` 已是最新 / `1` 已导入 / `-1` 卡上没有 `ui.bin` / `-2` 头非法 /
`-3` 擦写失败。

## 6. 模块里能做/不能做

- ✅ 画屏（通过自己的 SPI 驱动；SPI4 的引脚 PE10–PE15 目前没被固件占用）
- ✅ 读 `get_status()` 拿 CPU/声部/音色/库信息，用 `set_channel_patch()` 切音色
- ✅ 用 `set_probe()` 回报两个数字（`tools\status.ps1` 直接显示，调试很方便）
- ❌ 不能用 libc / 浮点打印 / malloc（要自己做，或只算整数）
- ❌ 不能注册中断（没有 startup；需要节拍就用 `tick()`）
- ❌ 不能引用宿主符号（链接时也不会给你符号表）

## 7. 当前进度

- ✅ 框架：`module_api.h` / `module.c` / `module.h` / `patch_storage_sd.c` 导入 / CMake 目标 /
  `tools/make_module.py` / 状态字段与 `status.ps1` 解码
- ✅ 最小测试模块 `Synth/module/ui.c`（tick 计数 + 按键计数 → probe）
- ✅ ST7735 TFT UI（三页：状态仪表盘 / 采样库信息 / 设置，PC13 翻页）
- ✅ **USB 上传**（见 §8）：改 UI 不用拔卡、不用烧写，一条命令 0.3 秒
- ⏳ 下一步：设置页做成可交互（改音量/效果）、更漂亮的布局
- ⚠ 内部 flash 已到 99.8%（调试构建）：再往固件里加东西之前先 `tools\build.ps1 -Trim`
  或删诊断代码；模块本身不受这个限制

## 8. USB 上传（开发迭代用，取代"拔卡拷 ui.bin"）

固件里内置了一条 SysEx 上传通道（`Synth/port/module_upload.c`），复用现有的 USB-MIDI 端点，
不额外占 flash（对比：加一个厂商自定义类要 2~3 KB，而内部 flash 只剩几百字节）。

```powershell
python tools\upload_module.py              # 默认上传 build\RelWithDebInfo\ui.bin
python tools\upload_module.py --list       # 列出 MIDI 输出设备（本板 = index 1）
python tools\upload_module.py --file path\ui.bin --verbose
```

流程：`开始（长度）` → 按 240 B 分块写 `镜像` → `提交（校验头 + CRC，重新加载模块）`。
上传期间主循环会停约 1 秒（QSPI 擦写是轮询的），声音断一下属正常。

协议（所有字节 ≤ 0x7F，十六进制 ASCII）：

| 消息 | 含义 |
|---|---|
| `F0 7D 00 <len:8 hex> F7` | 开始：擦掉 `len` 字节需要的扇区 |
| `F0 7D 01 <off:6 hex> <data hex…> F7` | 数据块（offset 是镜像内偏移） |
| `F0 7D 02 F7` | 提交：回读校验 + `synth_module_unload()` + `synth_module_init()` |
| 其它 `F0 …` | 吞掉（引擎本来也丢弃 SysEx） |

两个坑（都已修，值得记住）：

1. **十六进制解码的"半个字节"状态**：收完 6 位偏移后必须把 `s_hex_n/s_nibble` 清零，
   否则数据阶段变成"1 个字符 = 1 字节"，写进去的是错位垃圾。定位方法：上传
   `00 01 02 03 …` 的斜坡，读回却是 `00 10 20 30 …`（正好差一个字符）。
2. **`synth_module_init()` 是幂等的**：已加载就直接 `return 0`。上传后必须先
   `synth_module_unload()`，否则跑的还是旧镜像（症状：`module_crc` 不变、屏幕不刷新）。
   另外 QSPI 映射区是**可缓存**的 —— 校验走间接读（`synth_port_qspi_read`），
   或者读之前 `synth_port_qspi_cache_invalidate()`，否则读到缓存里的旧行。

上传过的模块会在头部打 `0x55534255`（"USB"）标记（`SYNTH_MODULE_USB_FLAG`）：开机时
`synth_port_module_import_from_card()` 看到标记就**不会**用卡上的旧 `ui.bin` 覆盖它。
要通过卡更新，删掉卡上的文件（或改标记）即可。

3. **标记的偏移写错过**（症状：USB 上传后正常，一重启"UI 又乱码了"）：
   `tools/upload_module.py` 打的是头部偏移 **32**（`reserved[1]`），固件早先查的是
   `reserved[0]`（偏移 **28**）⇒ 判定永不成立，每次上电都被卡上的旧 `ui.bin` 顶回去。
   现在固件两个字段都认。**自检办法**：比对 `status.ps1` 的 `module_crc` 与
   `tools\make_module.py` 输出/`ui.bin` 的真实 CRC —— 不一致就是被卡上的文件覆盖了。
