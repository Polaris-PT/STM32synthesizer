/* 通过 USB-MIDI SysEx 上传 RAM overlay 模块镜像（免拔卡/免烧写）

   为什么走 MIDI：板子上只有一个 USB 口，TinyUSB 已经在跑 USB-MIDI，加一条
   厂商自定义通道要多吃 2~3 KB flash（内部 flash 只剩 1 KB）。SysEx 是现成的
   免费数据通道，主机端用 winmm 的 midiOutLongMsg 就能发。

   协议（所有数据字节 ≤ 0x7F，十六进制 ASCII 编码）：
     F0 7D 00 <len:8 hex> F7                    ← 开始：len = 镜像总字节数（头+镜像）
     F0 7D 01 <off:6 hex> <data:hex...>  F7     ← 数据块：off = 镜像内偏移，data = 十六进制
     F0 7D 02 F7                                ← 提交：校验头/CRC 后重新加载模块
   非 0x7D 开头的 SysEx 一律吞掉（引擎本来也是丢弃 SysEx，行为不变）。

   写入细节：QSPI 页编程只能按 256 B 页写、且必须页对齐，所以这里攒满一页再发；
   头部（偏移 0）同样按页写 —— 中断时留下半截镜像，下次上传会重新擦写。 */
#ifndef SYNTH_PORT_MODULE_UPLOAD_H
#define SYNTH_PORT_MODULE_UPLOAD_H

#include <stdint.h>

/* 喂一个 MIDI 字节。返回 1 = 这个字节被上传协议吃掉了（调用方不要再送给合成引擎） */
int synth_port_module_upload_byte(uint8_t b);

/* 上一次提交的结果：0 = 模块已重新加载；<0 = 失败；0x100 = 本次上电还没提交过 */
int synth_port_module_upload_result(void);

/* 调试用：状态机 / 首次错误 / 已写字节数打包在一个 u32 里
   (bit0-7 状态机, bit8-15 首次错误, bit16-31 已收字节数上限 64 KB) */
uint32_t synth_port_module_upload_state(void);

#endif /* SYNTH_PORT_MODULE_UPLOAD_H */
