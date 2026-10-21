/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : H750VBT6 合成器主程序（CubeMX 生成骨架 + 应用层集成）
  ******************************************************************************
  * 板级约定（详见 docs/cubemx-checklist.md、docs/code-map.md）：
  *   SYSCLK   = 480 MHz（PLL1: M=5 N=192 P=2，VOS0 + FLASH_LATENCY_4）
  *   HCLK     = 240 MHz（AHB DIV2）
  *   USB 时钟  = PLL1Q = 48.000 MHz（.ioc 里 DIVQ1 = 20，CubeMX 生成的 PLLQ 也是 20，
  *                                    重新生成会保持）
  *   音频时钟  = PLL2P = 24.576 MHz（PLL2: M=5 N=108 FRACN=1101 P=22，写在 Core/Src/sai.c
  *                                    的 HAL_SAI_MspInit 里）→ MCKDIV = 2 → MCLK 12.288 MHz
  *                                    = 256×fs → 48.000 kHz
  *
  * 分工（CubeMX 重新生成 main.c 会覆盖「生成区」，原样保留 USER CODE 区）：
  *   .ioc / sai.c / dma.c / sdmmc.c / usart.c → 时钟树、SAI 格式、DMA、SDMMC、FATFS、串口
  *   SystemClock_Config() / MPU_Config()      → 生成区（PLL1、总线时钟、MPU Region0/1/2）
  *   本文件的 USER CODE 区                     → main()、QSPI 进内存映射、MCLK/GPIO 补丁、自检、
  *                                              .dtcm_bss 清零、诊断探针
  *   Synth/port/                              → 引擎胶水层（D-Cache/DWT/USB/SAI DMA/SD/串口）
  *
  * 诊断代码清单（**可整体删除**，删掉后固件只剩功能代码；各项互不依赖，可逐项删）：
  *   ① g_app_status（32 字）  启动/运行状态快照，用 tools/status.ps1 读；主循环刷新
  *   ② g_fault（20 字）       HardFault 现场：App_HardFaultRecord* + App_FindFaultFrame + 闪灯
  *   ③ PE3 心跳灯             App_LedInit/App_LedTask（快闪=故障、1 Hz=USB 未枚举、2 s 脉冲=正常）
  *   ④ 存活探针               systick_ticks / loop_count / main_stage / usb_isr_depth / audio_isr_depth
  *   ⑤ USB 寄存器快照         App_SnapshotUsb（仅在未枚举时刷新）
  *   ⑥ 时钟自检               App_CheckSaiClock / App_UsbKernelClockHz
  *   ⑦ 串口日志               Synth/port/debug_uart.[ch] + 各处 dbg_log（开关 SYNTH_PORT_DEBUG_UART）
  *   ⑧ USB-MIDI 逐包打印      开关 SYNTH_PORT_MIDI_TRACE（默认 0）
  *   删 ①-⑥ 时把 main() / 主循环里对应的调用一并删掉即可。
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "fatfs.h"
#include "quadspi.h"
#include "sai.h"
#include "sdmmc.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "synth_port_config.h"  /* SYNTH_PORT_* 开关（SD 使能、缓冲尺寸）——synth_port.h 不含它 */
#include "synth_port.h"      /* Synth/port：应用层唯一入口（D-Cache/DWT/USB/SAI/SD） */
#include "qspi_flash.h"      /* QSPI 底层原语：0x90000000 映射 / 擦 / 写 / 缓存失效 */
#include "module.h"          /* RAM overlay 模块：加载/CRC/回调（TFT UI 走这条） */
#include "module_upload.h"   /* USB SysEx 上传模块镜像（免拔卡迭代 UI 用） */
#include "synth/synth.h"     /* 引擎公开 API（C ABI） */
#include "debug_uart.h"      /* 调试串口 USART1（CubeMX 生成：PB14=TX/PB15=RX），开关见 synth_port_config.h */
#include "usart.h"           /* MX_USART1_UART_Init() / huart1 */
#include <stdio.h>           /* snprintf：HardFault 记录拼日志用 */
#if SYNTH_PORT_USB_ENABLE
#include "tusb.h"            /* tud_mounted()：把枚举结果暴露到 g_app_status */
#endif
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* 上板诊断状态：调试器 / tools/status.ps1 读 g_app_status 即可定位问题（不接串口也行） */
typedef struct {
  uint32_t sysclk_hz;    /* SystemCoreClock（应为 480000000） */
  uint32_t hclk_hz;      /* HAL_RCC_GetHCLKFreq()（应为 240000000） */
  uint32_t usb_ck_hz;    /* OTG_FS 内核时钟（应为 48000000） */
  uint32_t audio_ck_hz;  /* SAI23 内核时钟（应为 24576000） */
  uint32_t sai_mckdiv;   /* 实际写进 SAI3_Block_B->CR1 的 MCKDIV（应为 2） */
  uint32_t qspi_id;      /* QSPI flash 的 JEDEC ID（W25Q64 = 0xEF4017） */
  uint32_t fault;        /* APP_FAULT_* 位或 */
  int32_t  synth_init;   /* synth_init() 返回值（0 = OK） */
  int32_t  usb_init;     /* synth_port_usb_init() 返回值 */
  int32_t  sd_mount;     /* synth_port_sd_mount() 返回值 */
  int32_t  audio_start;  /* synth_port_audio_start() 返回值（-1 = DMA 没起来） */
  uint32_t usb_mounted;  /* tud_mounted()：1 = 主机已完成枚举（Windows 里就能看到设备） */
  uint32_t midi_rx_bytes;/* USB-MIDI 收到的字节数（弹个键就涨；不涨 = 主机没发数据） */
  uint32_t boot_count;   /* 启动次数（.ram_section，只有断电才丢）：持续增长 = 固件在复位循环，
                            用 reset_flags / g_fault 判断是哪种复位 */
  uint32_t reset_flags;  /* 上一次复位原因的原始快照（RCC->RSR，读过即清；位定义见 stm32h750xx.h
                            的 RCC_RSR_*：PORRSTF/PINRSTF/BORRSTF/SFTRSTF/WWDGRSTF/IWDGRSTF/LPWRRSTF） */
  uint32_t usb_mount_count; /* 主机完成枚举的次数（tud_mount_cb）—— 反复增长 = 主机在反复重枚举 */
  uint32_t usb_umount_count;/* 掉线次数（tud_umount_cb） */

  /* ---- USB 诊断：区分「核没产生中断」「中断了但没谈成」「时钟或 PHY 没起来」 ---- */
  uint32_t usb_irq_count;  /* OTG_FS 中断次数：插着线却恒为 0 → DWC2 核根本没产生中断 */
  uint32_t usb_sel;        /* RCC_D2CCIP2R.USBSEL：USB 内核时钟源（1 = PLL1Q，3 = HSI48，0 = 关） */
  uint32_t usb_clk_rc;     /* HAL_RCCEx_PeriphCLKConfig() 的返回值（0 = HAL_OK） */
  uint32_t usb_gotgctl;    /* ↓ DWC2 寄存器快照（App_SnapshotUsb()，仅在未枚举时刷新） */
  uint32_t usb_gintsts;    /*   GINTSTS：bit0 CURMOD（0 = device 模式）、bit12 USBRST、bit13 ENUMDNE */
  uint32_t usb_dctl;       /*   DCTL：bit0 RWUSIG = 1 → 软断开（DP 上拉没接上，主机看不到设备） */
  uint32_t usb_dsts;       /*   DSTS：bit0 SUSPSTS = 1 → 总线挂起（主机还没开始枚举） */
  uint32_t usb_gccfg;      /*   GCCFG：bit16 PWRDWN = 1 → 内部 FS 收发器已上电 */
  uint32_t usb_gusbcfg;    /*   GUSBCFG：bit6 PHYSEL（1 = FS 串行收发器） */
  uint32_t usb_dcfg;       /*   DCFG：bit0-1 DSPD（设备速度） */
  uint32_t loop_count;     /* 主循环圈数：不涨 = 主循环卡死或根本没跑到 */

  /* ---- 存活探针：间隔几秒连读两次，比较 systick_ticks / loop_count ----
       systick 不涨、loop_count 不涨 → CPU 停在优先级 0 的中断里（SysTick 优先级 15 被屏蔽），
                                        或卡在 Error_Handler 的 while(1)；用下面两个 depth 定位
       systick 涨、loop_count 不涨   → 主循环卡在某个阻塞调用（main_stage 指出卡在哪一步）
       两者都涨                       → 固件活着，问题在 USB 协议/主机侧 */
  uint32_t systick_ticks;   /* SysTick 中断次数（1 kHz）：不涨 = 连时钟节拍都停了 */
  uint32_t main_stage;      /* 主循环当前步骤：1 usb_task / 2 patch_service / 3 synth_service / 0 其他 */
  uint32_t usb_isr_depth;   /* >0 = halt 那一刻正在 USB 中断里（正常一闪而过；配合 systick 判断是否卡死） */
  uint32_t audio_isr_depth; /* >0 = 正在音频 DMA 中断里（改架构后只搬数据，正常也是几十微秒） */

  /* ---- 负载指标：判断"复音一多就滋滋/卡死"是不是 CPU 超预算 ----
       预算：128 帧 @48 kHz = 2667 us = 480 MHz 下约 1,280,000 周期（每半缓冲）。
       render_peak_cycles 接近 1.28M → 中断里渲染会撑爆半缓冲 → 断续/丢 MIDI 包；
       cpu_permille 稳定在 600 以上就没有余量了。 */
  uint32_t cpu_permille;        /* 引擎渲染负载（千分比：1000 = 正好用满实时预算） */
  uint32_t render_peak_cycles;  /* 单块渲染的最大周期数（上电以来） */
  uint32_t audio_underruns;     /* 预渲染环来不及填、音频中断重复上一块的次数 */

  /* ---- 多音色：16 个通道当前各自的音色号，每通道 4 bit 打包（0..14，15 = 未知）----
     验证：在 ch0 发 Program Change 5、ch1 发 8，这两个 nibble 应分别变成 5 和 8。 */
  uint32_t chan_patch[4];

  /* ---- SD 卡：挂载成功后的读写自检 + 内置音色导出结果 ----
     位含义（详见 Synth/port/patch_storage_sd.c 顶部注释）：
       bit0 目录可用 / bit1 至少写成功一个文件 / bit2 读回成功 / bit3 读回的文本能解析成音色 /
       bit4 跳过过已存在的文件（卡上本来就有，没被覆盖）
     新卡第一次上电期望 0x1F、sd_exported = 15；之后每次上电 sd_exported = 0。 */
  uint32_t sd_selftest;
  uint32_t sd_exported;
  uint32_t sd_dat3;      /* 历史字段，恒为 0：0 = DAT3 已交回 SDMMC 外设（4-bit 正常） */
  uint32_t sd_err;       /* 最后一次 SD 操作失败时 hsd1.ErrorCode（HAL_SD_ERROR_* 位）：
                            4 = CMD_RSP_TIMEOUT、8 = DATA_TIMEOUT、32 = RX_OVERRUN、1 = CMD_CRC_FAIL … */
  uint32_t sd_wtest;     /* SD「卡上内容说了算」写回自检位图（Synth/port/patch_storage_sd.c）：
                            bit0 覆盖写成功 / bit1 读回解析出的 cutoff = 测试值（说明卡上内容生效）/
                            bit2 恢复原件成功 / bit3 恢复后读回解析出的 cutoff = 内置值 */
  uint32_t qspi_wtest;   /* QSPI 数据通路自检：0 = 擦除 + 页编程 + 内存映射读回全部一致；
                            负数 = 第几步失败（-1/-4 写使能、-2 擦除命令、-3/-7 WIP 超时、-5/-6 页编程、-8 回映射、-9 读回不一致） */
  uint32_t qspi_wtest_rd;/* 从 0x90000000 + 0x7FF000 读回的头 4 字节（期望 0x514E5953 = "SYNQ" 小端） */
  /* --- 第二档：MIDI 控制面 / 持久化 ------------------------------------- */
  uint32_t nrpn_used;    /* 收到并命中映射表的 NRPN 数（Synth/src/core/engine.cpp 的 ApplyNrpn） */
  uint32_t nrpn_unmapped;/* 收到但没进映射表的 NRPN 数（>0 说明上位机在发我们还不认识的号） */
  uint32_t last_nrpn;    /* 最近一条 NRPN：MSB<<24 | LSB<<16 | 14 位值；0 = 还没收到 */
  uint32_t state_load;   /* /synth/state.cfg 恢复结果：-1(0xFFFFFFFF) = 没文件，其余 = 恢复的通道数 */
  uint32_t state_saves;  /* 上电以来把通道音色映射写回卡上的次数 */
  /* --- 第三档：QSPI 采样库（docs/sample-library.md）--------------------- */
  uint32_t lib_count;    /* QSPI 库里的乐器数（0 = 没有有效库） */
  uint32_t lib_serial;   /* 库的 serial（PC 打包时按内容生成；用来判断要不要重新导入） */
  uint32_t lib_import;   /* synth_port_lib_import_from_card() 返回值：0 = 已是最新/导入成功，
                            -1 卡上没 samples.bin、-2 头非法、-3 擦除失败、-4 写入失败、-5 校验失败 */
  /* --- RAM overlay 模块（TFT UI 等，见 Synth/port/module.h）：不占内部 flash ---------- */
  uint32_t module_import; /* synth_port_module_import_from_card() 返回值：0 已最新 / 1 已导入 / <0 失败 */
  int32_t  module_state;  /* synth_module_init() 返回：0 = 已加载，-1..-5 失败原因（见 module.h） */
  uint32_t module_crc;    /* 已加载模块镜像的 CRC32（无模块时 0） */
  uint32_t module_ticks;  /* 模块 tick 被调用的次数（不涨 = 模块自己卡住了） */
  uint32_t module_probe[2]; /* 模块通过 api->set_probe() 回报的两个值（由模块自己定义） */
  uint32_t up_state;      /* USB SysEx 上传状态机（调试用）：bit0-7 状态、bit8-15 首次错误、bit16-31 已写字节 */
} app_status_t;

/* ---- HardFault 现场记录 ------------------------------------------------------
   CubeMX 生成的 HardFault_Handler 只是 while(1)，不接调试器什么都看不到；而"单步没事、全速跑
   就进 HardFault"这类问题多半发生在中断里 —— 单步会改变中断时序，现场就消失了。所以让固件
   自己把现场记下来（tools/status.ps1 直接读成人话）。放在 .ram_section（NOLOAD）：启动代码
   既不加载也不清零它 → 复位后现场还在（事后验尸），且不占 .bss。
   magic == APP_FAULT_MAGIC 才代表下面字段有效（magic = 0 = 本次上电没崩过）。 */
typedef struct {
  uint32_t magic;       /* APP_FAULT_MAGIC = 有效 */
  uint32_t count;       /* 累计进入 HardFault 的次数（反复发生 = 不是偶发） */
  uint32_t cfsr;        /* SCB->CFSR：合并的 MemManage/BusFault/UsageFault 状态 */
  uint32_t hfsr;        /* SCB->HFSR：bit30 FORCED=1 → 由上面三个 fault 升级而来；bit1 VECTTBL */
  uint32_t mmfar;       /* SCB->MMFAR：CFSR.MMARVALID=1 时 = 触发 MemManage 的地址 */
  uint32_t bfar;        /* SCB->BFAR：CFSR.BFARVALID=1 时 = 触发 BusFault 的地址 */
  uint32_t icsr;        /* SCB->ICSR：此刻 VECTACTIVE 恒为 3(HardFault)，VECTPENDING 有意义 */
  uint32_t exc_return;  /* 出事时的 LR：bit2=0 → 当时在中断里（栈在 MSP）；=1 → 线程模式（栈在 PSP） */
  uint32_t sp;          /* 硬件压栈的栈帧地址（frame_valid=1 时有效） */
  uint32_t frame_valid; /* 1 = 下面 r0..xpsr 是真正的现场 */
  uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;  /* 硬件压栈的那 8 个字 */
  uint32_t msp, psp;    /* 出事瞬间的两个栈指针 */
} app_fault_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* 音频时钟树：24.576 MHz = 512 × 48 kHz；MCKDIV = 24576000/(48000×256) = 2 */
#define APP_AUDIO_FS_HZ     48000u
#define APP_AUDIO_CK_HZ     24576000u
#define APP_AUDIO_CK_DIV    (APP_AUDIO_FS_HZ * 256u)   /* MCKDIV 分母（OSR 关闭） */

/* QSPI 内存映射读（0xEB 四线快读）的空周期数。W25Q64 时序：指令(1线) → 24 位地址(4线) →
   模式字节(4线) → 4 空周期 → 数据(4线)。与 WeAct 官方 BSP 的 QSPI_EnableMemoryMappedMode()
   一致（它的"指令 1 线"分支也是 4，只有 QPI 模式才用 6）。读出的采样若整体错位/全 0xFF，改成 6 再试。 */
#define APP_QSPI_DUMMY_CYCLES   4u
#define APP_QSPI_MAP_BASE       0x90000000u   /* QSPI 内存映射窗口基址（与 sample_storage_qspi.c 一致） */

/* PLL2 由 CubeMX 生成的代码负责（Core/Src/sai.c 的 HAL_SAI_MspInit：5 MHz × (108 + 1101/8192)
   / 22 = 24.57599987 MHz，误差 −0.13 Hz），这里不再重复配置。 */

/* g_app_status.fault 位定义 */
#define APP_FAULT_SAI_INIT      (1u << 0)   /* HAL_SAI_InitProtocol() 失败 */
#define APP_FAULT_SAI_CLK       (1u << 1)   /* SAI3 内核时钟偏离 24.576 MHz 超过 ±0.1% */
#define APP_FAULT_SAI_DMA       (1u << 2)   /* CubeMX 未给 SAI3_B 配 DMA（hdmatx == NULL） */
#define APP_FAULT_SYNTH_INIT    (1u << 3)
#define APP_FAULT_USB_INIT      (1u << 4)
#define APP_FAULT_AUDIO_START   (1u << 5)
#define APP_FAULT_SD_MOUNT      (1u << 6)
#define APP_FAULT_HAL           (1u << 7)   /* 走到 Error_Handler() */
#define APP_FAULT_QSPI_MM       (1u << 8)   /* QSPI 没能进内存映射（外部采样库不可用） */

/* g_fault.magic 的有效标志（"HFZ1"）——没发生过 HardFault 时这块内存里是随机值 */
#define APP_FAULT_MAGIC         0x48465A31u

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile app_status_t g_app_status;

/* 复位后不清零的启动计数：.ram_section 是 NOLOAD —— 启动代码既不加载也不清零它，
   只有真正断电才丢。用它区分「固件跑起来了但 USB 没起来」和「固件在复位循环」。 */
#define APP_BOOT_MAGIC 0x53594E54u   /* "SYNT" */
static volatile uint32_t g_boot_magic __attribute__((section(".ram_section")));
static volatile uint32_t g_boot_count __attribute__((section(".ram_section")));

/* HardFault 现场（.ram_section → 复位后仍可读，见 app_fault_t 的说明） */
volatile app_fault_t g_fault __attribute__((section(".ram_section")));
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
/* USER CODE BEGIN PFP */
static void App_InitAudioPath(void);
static void App_CheckSaiClock(void);
static void App_SaiGpioSpeed(void);
static void App_ZeroDtcmBss(void);
static int  App_QspiEnterMemoryMapped(void);
static int  App_QspiWriteTest(void);
static void App_Fault(uint32_t bit);
static void App_RecordBoot(void);
static void App_LedInit(void);
static void App_LedTask(void);
static void App_KeyInit(void);
static void App_KeyTask(void);
static uint32_t App_UsbKernelClockHz(void);
static void App_SnapshotUsb(void);

/* HardFault 记录：定义在下面，被 Core/Src/stm32h7xx_it.c 的 HardFault_Handler 调用。
   ⚠ App_HardFaultRecordC 必须是外部链接（不能 static）：它只被 naked 函数里的内联汇编
      按名字引用，GCC 看不见这次引用，会当成"未使用的 static 函数"直接丢掉 → 链接报
      undefined reference。 */
void    App_HardFaultRecord(void);                                        /* naked，不返回 */
void    App_HardFaultRecordC(uint32_t exc_return, uint32_t msp, uint32_t psp);
static uint32_t *App_FindFaultFrame(uint32_t *sp);
static void App_FaultBlinkForever(void) __attribute__((noreturn));
static void App_FaultDelay(volatile uint32_t loops);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void App_Fault(uint32_t bit)
{
  g_app_status.fault |= bit;
}

/* 复位计数 + 复位原因。必须在最前面调用（在任何可能崩掉的初始化之前）。
   boot_count 持续增长 = 板子在复位循环（主机侧表现成「设备起来又没了」= 枚举失败 / 代码 10）；
   原因看 reset_flags（SFTRSTF = 软复位，多半来自 HardFault 后的复位策略）或 g_fault。 */
static void App_RecordBoot(void)
{
  const uint32_t flags = RCC->RSR;        /* 上一次复位的原因（位定义见 stm32h750xx.h 的 RCC_RSR_*） */

  __HAL_RCC_CLEAR_RESET_FLAGS();          /* 清掉，下次读到的才是「上一次」的原因 */

  if (g_boot_magic != APP_BOOT_MAGIC) {   /* 刚上电：.ram_section 里是垃圾值 */
    g_boot_magic = APP_BOOT_MAGIC;
    g_boot_count = 1u;
  } else {
    ++g_boot_count;                       /* 软复位 / 看门狗复位会走到这里 */
  }

  g_app_status.boot_count  = g_boot_count;
  g_app_status.reset_flags = flags;

  /* 开机清掉 HardFault 记录：让「本次上电没崩过」和「AXI 里的随机残留」能区分开。
     刚烧写完 / 断电再上电时 .ram_section 里是垃圾值（曾把 0x0A 误读成 magic），
     清零后 status.ps1 读到 magic = 0 就能确定「这一轮真的没进过 HardFault」。 */
  g_fault.magic = 0u;
  g_fault.count = 0u;
}

/* ---- 板载 LED（PE3）心跳 ----------------------------------------------------
   不接调试器也能一眼看出固件活到哪一步。闪烁模式编码状态（下面是"亮"的条件）：
     fault != 0        → 100 ms 快闪        （有故障位）
     USB 未枚举        → 每 1 s 亮 500 ms   （慢闪：主循环在跑，但 USB 没起来）
     USB 已枚举        → 每 2 s 亮 100 ms   （短脉冲：一切正常）
   PE3 在 WeAct 板上就是 LED（资料\MiniSTM32H7xx-master\...\06-SPIFlash_Test 的
   LED_Blink 用的也是 PE3_Pin），与 QSPI 的 PE2(IO2) 相邻但不冲突。 */
static void App_LedInit(void)
{
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef led = {0};
  led.Pin   = GPIO_PIN_3;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &led);
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET);
}

static void App_LedTask(void)
{
  const uint32_t now = HAL_GetTick();
  GPIO_PinState on;

  if (g_app_status.fault != 0u) {                 /* 有故障：快闪，最醒目 */
    on = ((now / 100u) & 1u) ? GPIO_PIN_SET : GPIO_PIN_RESET;
  } else if (g_app_status.usb_mounted == 0u) {    /* USB 没起来：1 Hz 慢闪 */
    on = ((now % 1000u) < 500u) ? GPIO_PIN_SET : GPIO_PIN_RESET;
  } else {                                        /* 正常：每 2 秒一个短脉冲 */
    on = ((now % 2000u) < 100u) ? GPIO_PIN_SET : GPIO_PIN_RESET;
  }
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, on);
}

/* ── 用户键（PC13，**按下拉高**）+ 事件转发给 RAM overlay 模块 ────────────────────
   该板 K1 接在 PC13，按下时把引脚拉高（不是拉低！）。所以配置必须用下拉：
   若用上拉，引脚永远读 1，按键事件一个也收不到（症状：屏幕不翻页、module_probe[1] 恒 0）。
   20 ms 消抖；只在"稳定状态变化"时回调模块（模块可以拿它做翻页/确认）。 */
static void App_KeyInit(void)
{
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIO_InitTypeDef key;
  key.Pin   = GPIO_PIN_13;
  key.Mode  = GPIO_MODE_INPUT;
  key.Pull  = GPIO_PULLDOWN;       /* 按下 = 高 ⇒ 平时靠下拉保持 0 */
  key.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &key);
}

static void App_KeyTask(void)
{
  static uint32_t s_change_ms;
  static uint8_t  s_raw = 0u;      /* 0 = 松开（下拉） */
  static uint8_t  s_stable = 0u;

  const uint8_t raw = (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET) ? 1u : 0u;
  const uint32_t now = HAL_GetTick();

  if (raw != s_raw) {              /* 抖：重新计时 */
    s_raw = raw;
    s_change_ms = now;
    return;
  }
  if (s_stable != raw && (uint32_t)(now - s_change_ms) >= 20u) {
    s_stable = raw;
    synth_module_key((s_stable == 1u) ? 1u : 0u);   /* 高 = 按下 → 传 1 */
  }
}

/* USB 内核时钟频率。⚠ 不能用 HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USB)：
   H7 这一版 HAL 在该函数的 if 链里【没有 USB 分支】（stm32h7xx_hal_rcc_ex.c:1881 起，
   只有 SAI1/SAI2/…/SDMMC 等），会返回 0 —— 把"时钟正常"误报成"USB 时钟为 0"。
   这里按 mux（USBSEL）+ PLL1Q 自己算。 */
static uint32_t App_UsbKernelClockHz(void)
{
  const uint32_t src = __HAL_RCC_GET_USB_SOURCE();

  if (src == RCC_USBCLKSOURCE_HSI48) {
    return 48000000u;
  }
  if (src == RCC_USBCLKSOURCE_PLL) {          /* 我们的配置：PLL1Q = 960/20 = 48 MHz */
    PLL1_ClocksTypeDef pll1;
    HAL_RCCEx_GetPLL1ClockFreq(&pll1);
    return pll1.PLL1_Q_Frequency;
  }
  return 0u;                                  /* 源被关掉（USBSEL = 0）或用了 PLL3 */
}

/* DWC2 寄存器实时快照（只读）。判读要点：
     usb_gotgctl bit19 BSESVLD = 0 → VBUS 没被识别（线/口/供电，或 VBUS 检测没开）
     usb_gintsts bit0  CURMOD  = 1 → 核跑在 host 模式（配置错了）
     usb_dsts    bit0  SUSPSTS = 1 → 总线挂起（主机没开始枚举 / 线只供电不通讯）
     usb_dctl    bit0  RWUSIG  = 1 → 软断开：DP 上拉没接上，主机永远看不到设备
     usb_gccfg   bit16 PWRDWN  = 0 → 内部 FS 收发器被断电（TinyUSB 会置 1）
     usb_irq_count = 0 而线插着  → 核没产生任何中断（时钟/PHY/中断使能问题） */
static void App_SnapshotUsb(void)
{
  USB_OTG_GlobalTypeDef *usb  = USB_OTG_FS;
  /* DCFG/DCTL/DSTS 是"设备模式寄存器"，不在全局结构体里，偏移 USB_OTG_DEVICE_BASE(0x800)
     （stm32h750xx.h:1977 的 USB_OTG_DeviceTypeDef；HAL 的 PCD 驱动也是这么访问的）。 */
  USB_OTG_DeviceTypeDef *usbd = (USB_OTG_DeviceTypeDef *)((uint32_t)USB_OTG_FS + USB_OTG_DEVICE_BASE);

  g_app_status.usb_sel       = (RCC->D2CCIP2R & RCC_D2CCIP2R_USBSEL) >> RCC_D2CCIP2R_USBSEL_Pos;
  g_app_status.usb_irq_count = g_synth_usb_irq_count;
  g_app_status.usb_gotgctl   = usb->GOTGCTL;
  g_app_status.usb_gintsts   = usb->GINTSTS;
  g_app_status.usb_dctl      = usbd->DCTL;
  g_app_status.usb_dsts      = usbd->DSTS;
  g_app_status.usb_gccfg     = usb->GCCFG;
  g_app_status.usb_gusbcfg   = usb->GUSBCFG;
  g_app_status.usb_dcfg      = usbd->DCFG;
}

/* SAI3 时钟自检（只读，不改任何寄存器 —— PLL2/MCKDIV 交给 CubeMX 生成的代码）：
     sai.c 的 HAL_SAI_MspInit() 配 PLL2P = 24.57599987 MHz 并选 SAI23SEL ← PLL2P，
     HAL_SAI_Init() 再按当时的内核时钟算出 MCKDIV = 2 → MCLK 12.288 MHz = 256×fs。
   容差 ±0.1%：24.576 MHz 只能用 PLL2 的 FRACN 逼近，量化后必然差几 Hz（本配置 −0.13 Hz），
   用等号判断会每次开机误报 APP_FAULT_SAI_CLK。 */
static void App_CheckSaiClock(void)
{
  const uint32_t ck = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI3);

  g_app_status.audio_ck_hz = ck;
  g_app_status.sai_mckdiv  = (READ_REG(hsai_BlockB3.Instance->CR1) & SAI_xCR1_MCKDIV)
                             >> SAI_xCR1_MCKDIV_Pos;

  if ((ck < (APP_AUDIO_CK_HZ - (APP_AUDIO_CK_HZ / 1000u))) ||
      (ck > (APP_AUDIO_CK_HZ + (APP_AUDIO_CK_HZ / 1000u)))) {
    App_Fault(APP_FAULT_SAI_CLK);      /* 时钟不对：会走音，但先让它出声便于排查 */
  }
}

/* PD8=SCK / PD9=SD / PD10=FS / PD14=MCLK：CubeMX 生成的 MspInit 把它们配成
   GPIO_SPEED_FREQ_LOW，而 MCLK/SCK 是 12.288 MHz，LOW 档（数据手册上限约 12 MHz、
   且边沿很钝）会让 PCM5102A 收到畸形的位时钟。这里保持 AF6 不变，只把速度提到 VERY_HIGH。
   CubeMX 侧同步改：Pinout → SAI3 引脚 → GPIO Settings → Maximum output speed = Very High。 */
static void App_SaiGpioSpeed(void)
{
  GPIO_InitTypeDef gpio = {0};

  gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_14;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF6_SAI3;
  HAL_GPIO_Init(GPIOD, &gpio);
}

/* PD4 = MicroSD_SW 的卡检测上拉已经在 CubeMX 里配好了（Core/Src/gpio.c 里是 GPIO_PULLUP），
   这里不再重复配置：无卡 = 高 = SD_NOT_PRESENT，插卡 = 低 = SD_PRESENT
   （与 FATFS/Target/fatfs_platform.c:21 的判据一致）。 */

/* .dtcm_bss（链接脚本 STM32H750xx_FLASH.ld 里的 DTCM 热点段）是 NOLOAD：
   既不占 flash，启动代码也不清零（启动只清 .tbss/.bss，那两个段在 AXI SRAM）。
   所以凡是标注成 section(".dtcm_bss") 的数组，都要靠这里清零 —— 在 main() 里、
   在任何人使用它们之前调用一次。
   注意：C++ 全局构造（__libc_init_array）跑在 main() 之前，构造里不要读本段的变量。 */
extern uint32_t __dtcm_bss_start[];
extern uint32_t __dtcm_bss_end[];

static void App_ZeroDtcmBss(void)
{
  uint32_t *p = __dtcm_bss_start;

  while (p < __dtcm_bss_end) {
    *p++ = 0u;
  }
}

/* 把外部 QSPI flash 切进内存映射（XIP）模式 —— CubeMX 不管这一步，只能手写。
   为什么必须有：Synth/port/sample_storage_qspi.c 是零拷贝的，它把采样数据指针直接指到
   0x90000000 + offset（SYNTH_QSPI_MAP_BASE），全程没有任何 HAL_QSPI 读操作；
   缺了这一步，读那个窗口只会读到垃圾（MPU Region2 已把该窗口设成非缓存只读，
   所以读到的一定是 flash 的真实内容，不会读到 D-Cache 里的陈旧行）。
   板载 flash = W25Q64 系列（8 MB，与 .ioc 的 QUADSPI.FlashSize = 23 一致），序列：
     1) 0x66 + 0x99 软复位；
     2) 0x9F 读 JEDEC ID（自检：ID 不对就说明引脚/时钟/CS 有问题）；
     3) 读 SR2 的 QE 位（bit1）；为 0 就 0x06 写使能 + 0x31 写 SR2 置位，再等 WIP 清零
        —— QE=0 时 IO2/IO3 还当 WP#/HOLD# 用，四线读必然错；
     4) HAL_QSPI_MemoryMapped()：0xEB 四线快读（1-4-4），模式字节 0x00 = 不启用连续读，
        超时计数关闭（只读访问、写由 MPU 挡）。
   返回 0 = 成功；负数 = 停在第几步，调用方把它记进 APP_FAULT_QSPI_MM。 */
static int App_QspiEnterMemoryMapped(void)
{
  /* 统一走 qspi_flash.c 的实现：只有那里维护 s_mmap 状态，间接读/擦写之后才能把
     内存映射恢复回来。main.c 自己再实现一份会让状态跟踪失效 —— 2026-10-20 的
     模块加载 BusFault（BFAR = 0x907F0000）就是这条链：导入器 skip 分支间接读之后
     控制器留在了空闲态，随后用映射指针读头部就吃了 AHB 错误。 */
  const int rc = synth_port_qspi_enter_mmap();
  g_app_status.qspi_id = synth_port_qspi_id();
  return rc;

  /* 以下为旧的内联实现（已由上面一行取代，保留作对照，不参与编译） */
#if 0
  QSPI_CommandTypeDef      cmd = {0};
  QSPI_MemoryMappedTypeDef mm  = {0};
  uint8_t  id[3] = {0u, 0u, 0u};
  uint8_t  sr2   = 0u;
  uint8_t  sr1   = 0xFFu;
  uint8_t  value = 0u;
  uint32_t guard;

  /* 所有命令的公共部分：单线指令、无地址/无数据（下面按需覆盖） */
  cmd.InstructionMode    = QSPI_INSTRUCTION_1_LINE;
  cmd.AddressMode        = QSPI_ADDRESS_NONE;
  cmd.AddressSize        = QSPI_ADDRESS_24_BITS;
  cmd.AlternateByteMode  = QSPI_ALTERNATE_BYTES_NONE;
  cmd.AlternateBytes     = 0x00u;
  cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd.DataMode           = QSPI_DATA_NONE;
  cmd.NbData             = 0u;
  cmd.DummyCycles        = 0u;
  cmd.DdrMode            = QSPI_DDR_MODE_DISABLE;
  cmd.DdrHoldHalfCycle   = QSPI_DDR_HHC_ANALOG_DELAY;
  cmd.SIOOMode           = QSPI_SIOO_INST_EVERY_CMD;

  /* 1) 软复位：0x66（Enable Reset）+ 0x99（Reset Device）。失败不致命，继续往下试 */
  cmd.Instruction = 0x66u;
  (void)HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE);
  cmd.Instruction = 0x99u;
  (void)HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE);
  HAL_Delay(2u);                              /* 复位期间不收命令（tRST） */

  /* 2) 读 JEDEC ID（3 字节）：W25Q64 = EF 40 17 */
  cmd.Instruction = 0x9Fu;
  cmd.DataMode    = QSPI_DATA_1_LINE;
  cmd.NbData      = 3u;
  if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
    return -1;
  }
  if (HAL_QSPI_Receive(&hqspi, id, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
    return -2;
  }
  g_app_status.qspi_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];

  /* 3) 确认 QE（Quad Enable，SR2 bit1） */
  cmd.Instruction = 0x35u;                    /* Read Status Register-2 */
  cmd.NbData      = 1u;
  if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
    return -3;
  }
  if (HAL_QSPI_Receive(&hqspi, &sr2, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
    return -4;
  }

  if ((sr2 & 0x02u) == 0u) {                  /* 需要置 QE */
    cmd.Instruction = 0x06u;                  /* Write Enable */
    cmd.DataMode    = QSPI_DATA_NONE;
    cmd.NbData      = 0u;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
      return -5;
    }

    value = (uint8_t)(sr2 | 0x02u);           /* SR2：只置 QE，其它位保持 */
    cmd.Instruction = 0x31u;                  /* Write Status Register-2（1 字节） */
    cmd.DataMode    = QSPI_DATA_1_LINE;
    cmd.NbData      = 1u;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
      return -6;
    }
    if (HAL_QSPI_Transmit(&hqspi, &value, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
      return -7;
    }

    /* 等 WIP（SR1 bit0）清零：写状态寄存器通常 < 10 ms */
    cmd.Instruction = 0x05u;                  /* Read Status Register-1 */
    cmd.NbData      = 1u;
    for (guard = 0u; guard < 100000u; ++guard) {
      if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return -8;
      }
      if (HAL_QSPI_Receive(&hqspi, &sr1, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        return -9;
      }
      if ((sr1 & 0x01u) == 0u) {
        break;
      }
    }
    if ((sr1 & 0x01u) != 0u) {
      return -10;                             /* 超时：flash 没应答（查 CS/时钟/供电） */
    }
  }

  /* 4) 进内存映射：0xEB 四线 I/O 快读 */
  cmd.Instruction        = 0xEBu;
  cmd.Address            = 0x00000000u;
  cmd.AddressMode        = QSPI_ADDRESS_4_LINES;
  cmd.AddressSize        = QSPI_ADDRESS_24_BITS;
  cmd.AlternateByteMode  = QSPI_ALTERNATE_BYTES_4_LINES;
  cmd.AlternateBytes     = 0x00u;             /* M7-M0 = 0：不进连续读模式 */
  cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd.DummyCycles        = APP_QSPI_DUMMY_CYCLES;
  cmd.DataMode           = QSPI_DATA_4_LINES;
  cmd.NbData             = 0u;

  mm.TimeOutActivation   = QSPI_TIMEOUT_COUNTER_DISABLE;
  mm.TimeOutPeriod       = 0u;

  if (HAL_QSPI_MemoryMapped(&hqspi, &cmd, &mm) != HAL_OK) {
    return -11;
  }

  return 0;
#endif
}

/* ---- QSPI 数据通路自检：擦一页 → 写图案 → 回内存映射 → 从 0x90000000 读回比对 --------
   为什么需要：在此之前只验证过"读 JEDEC ID"（0x9F），而**内存映射的数据通路**从没验证过；
   而 Synth/port/sample_storage_qspi.c 是零拷贝直接读 0x90000000 的 —— 这一步过了，
   8 MB QSPI 才能当采样/音色库用。用的是最后一个 4 KB 扇区（偏移 0x7FF000，目前没有任何数据）。
   结果写进 g_app_status.qspi_wtest（0 = 全对）/ qspi_wtest_rd。 */
#define APP_QSPI_WTEST_OFFSET  0x7FF000u
static const uint8_t kQspiPattern[16] = {
  0x53u, 0x59u, 0x4Eu, 0x51u, 0x00u, 0x11u, 0x22u, 0x33u,   /* "SYNQ" + 递增图案 */
  0x44u, 0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xAAu, 0xBBu
};

/* 填公共命令字段（单线指令 / 无数据），返回后调用方覆盖需要变的项 */
static void App_QspiCmdInit(QSPI_CommandTypeDef *cmd)
{
  memset(cmd, 0, sizeof(*cmd));
  cmd->InstructionMode    = QSPI_INSTRUCTION_1_LINE;
  cmd->AddressMode        = QSPI_ADDRESS_NONE;
  cmd->AddressSize        = QSPI_ADDRESS_24_BITS;
  cmd->AlternateByteMode  = QSPI_ALTERNATE_BYTES_NONE;
  cmd->AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
  cmd->DataMode           = QSPI_DATA_NONE;
  cmd->NbData             = 0u;
  cmd->DummyCycles        = 0u;
  cmd->DdrMode            = QSPI_DDR_MODE_DISABLE;
  cmd->DdrHoldHalfCycle   = QSPI_DDR_HHC_ANALOG_DELAY;
  cmd->SIOOMode           = QSPI_SIOO_INST_EVERY_CMD;
}

static int App_QspiWriteEnable(void)      /* 0x06：擦/写之前必须先发 */
{
  QSPI_CommandTypeDef cmd;
  App_QspiCmdInit(&cmd);
  cmd.Instruction = 0x06u;
  return (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK) ? 0 : -1;
}

static int App_QspiWaitWip(void)          /* 轮询 SR1.WIP(bit0)，最多 ~500 ms */
{
  QSPI_CommandTypeDef cmd;
  uint8_t sr1 = 0xFFu;
  App_QspiCmdInit(&cmd);
  cmd.Instruction = 0x05u;
  cmd.DataMode    = QSPI_DATA_1_LINE;
  cmd.NbData      = 1u;
  for (uint32_t i = 0u; i < 500u; ++i) {
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -1;
    if (HAL_QSPI_Receive(&hqspi, &sr1, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -2;
    if ((sr1 & 0x01u) == 0u) return 0;
    HAL_Delay(1u);
  }
  return -3;
}

/* 用开关 SYNTH_PORT_QSPI_WRITE_TEST 启用；关掉时保留代码但不参与编译（unused 抑制警告） */
static int __attribute__((unused)) App_QspiWriteTest(void)
{
  QSPI_CommandTypeDef cmd;
  uint8_t rd[16];

  /* 已经在内存映射状态时 HAL 不收普通命令，先退出来 */
  if (hqspi.State == HAL_QSPI_STATE_BUSY_MEM_MAPPED) {
    (void)HAL_QSPI_Abort(&hqspi);
  }

  /* 1) 擦除 4 KB 扇区（0x20 + 24 位地址） */
  if (App_QspiWriteEnable() != 0) return -1;
  App_QspiCmdInit(&cmd);
  cmd.Instruction = 0x20u;
  cmd.Address     = APP_QSPI_WTEST_OFFSET;
  cmd.AddressMode = QSPI_ADDRESS_1_LINE;
  if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -2;
  if (App_QspiWaitWip() != 0) return -3;

  /* 2) 页编程（0x02），16 字节 */
  if (App_QspiWriteEnable() != 0) return -4;
  cmd.Instruction = 0x02u;
  cmd.DataMode    = QSPI_DATA_1_LINE;
  cmd.NbData      = (uint32_t)sizeof(kQspiPattern);
  if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -5;
  if (HAL_QSPI_Transmit(&hqspi, (uint8_t *)kQspiPattern, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -6;
  if (App_QspiWaitWip() != 0) return -7;

  /* 3) 回内存映射模式（读回必须走映射路径，不然证明不了数据通路） */
  if (App_QspiEnterMemoryMapped() != 0) return -8;

  /* 3b) 0x90000000 现在是可缓存区：刚写进去的数据要先让缓存失效，否则读回的是旧行 */
  synth_port_qspi_cache_invalidate();

  /* 4) 从映射窗口读回比对 */
  {
    const volatile uint8_t *p = (const volatile uint8_t *)(APP_QSPI_MAP_BASE + APP_QSPI_WTEST_OFFSET);
    for (uint32_t i = 0u; i < sizeof(kQspiPattern); ++i) rd[i] = p[i];
  }
  g_app_status.qspi_wtest_rd = ((uint32_t)rd[0]) | ((uint32_t)rd[1] << 8) |
                               ((uint32_t)rd[2] << 16) | ((uint32_t)rd[3] << 24);
  for (uint32_t i = 0u; i < sizeof(kQspiPattern); ++i) {
    if (rd[i] != kQspiPattern[i]) return -9;
  }
  return 0;
}

/* 音频通路初始化：SAI3 Block B。CubeMX 侧已经配好 32bit / 2 slot / DMA 循环 WORD /
   NVIC 优先级 0 / PLL2 = 24.576 MHz，这里只补生成代码缺的两件事并做自检：
     1) Init.MckOutput：生成的初始化里没赋这个字段（默认 0 = MCK 不输出）→ PD14 上不会有 MCLK，
        而 PCM5102A 的 SCK 就接在 PD14；补上后重新 init 一次（格式与 .ioc 一致，无副作用）；
     2) GPIO 输出速度：生成的是 FREQ_LOW，驱动不了 12.288 MHz 的 MCLK/SCK。
   自检结果都写进 g_app_status，上板用调试器看一眼就知道哪一步没对。 */
static void App_InitAudioPath(void)
{
  MX_SAI3_Init();                       /* GPIO PD8/9/10/14 + SAI3 时钟 + PLL2 + DMA 链接 */

  hsai_BlockB3.Init.MckOutput = SAI_MCK_OUTPUT_ENABLE;   /* PD14 = MCLK = 12.288 MHz */
  if (HAL_SAI_InitProtocol(&hsai_BlockB3, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_32BIT, 2) != HAL_OK) {
    App_Fault(APP_FAULT_SAI_INIT);
  }

  App_SaiGpioSpeed();                   /* 必须在 init 之后：MspInit 每次都会重配 GPIO */
  App_CheckSaiClock();

  if (hsai_BlockB3.hdmatx == NULL) {    /* CubeMX 已配 SAI3_B → DMA1_Stream0，正常不会为空 */
    App_Fault(APP_FAULT_SAI_DMA);
  }
}

/* ============================ HardFault 现场记录 ============================
   被 Core/Src/stm32h7xx_it.c 的 HardFault_Handler 第一行调用；记录完闪灯并死循环，
   永不返回（所以它后面的 while(1) 不会执行到）。

   为什么要 naked：Cortex-M 进异常时硬件把 r0-r3,r12,lr,pc,xpsr 压到「出事那一刻的栈」上。
   若故障发生在中断里（本工程最可能：音频 DMA 中断优先级 0），那块栈是 MSP；普通 C 函数的
   序言一压栈 MSP 就变了，再也算不回准确位置。naked 函数没有序言，进来第一件事就是抓
   MSP/PSP/LR，然后「尾调用」交给 C 函数（bx 而不是 bl：不压栈、不动 MSP）。 */
__attribute__((naked, noreturn)) void App_HardFaultRecord(void)
{
  __asm volatile (
    "mrs  r1, msp                    \n"  /* r1 = 当前 MSP（HardFault_Handler 序言已 push {r3, lr}） */
    "ldr  r0, [r1, #4]               \n"  /* r0 = 真正的 EXC_RETURN：C 函数里 mov r0,lr 拿到的是 BL 返回地址，不是它 */
    "mrs  r2, psp                    \n"  /* r2 = 当前 PSP */
    "ldr  r3, =App_HardFaultRecordC  \n"
    "bx   r3                         \n"
  );
}

/* 在 MSP 往上（地址增大的方向）找硬件异常栈帧。判据：
     frame[7] = xPSR：bit24(T)=1（Thumb 态）且低 9 位 = 异常号 ≠ 0（说明出事时在中断里）
     frame[6] = PC  ：必须落在 Flash（0x08000000..0x08200000）
   最多找 64 个字（256 B）——足够跨过 handler 序言、BL 压的返回地址和调试器留下的内容。 */
static uint32_t *App_FindFaultFrame(uint32_t *sp)
{
  /* HardFault_Handler 的 C 前导先 push {r3, lr} 再 bl App_HardFaultRecord() ——
     所以 [sp+0] = 保存的 r3、[sp+1] = EXC_RETURN，异常栈帧从 [sp+2] 开始。
     不依赖这个布局：先找 EXC_RETURN 特征（0xFFFFFFE0 掩码全 1 且 bit0=1），帧在它后面 4 字节。 */
  for (uint32_t i = 0u; i < 64u; ++i) {
    const uint32_t v = sp[i];
    if (((v & 0xFFFFFFE0u) == 0xFFFFFFE0u) && ((v & 0x1u) != 0u)) {
      uint32_t *f = sp + i + 1u;
      /* 只校验 T 位 + PC 在 flash：线程模式出事时 xPSR 低 9 位（IPSR）= 0，不能再要求非 0 */
      if (((f[7] & 0x01000000u) != 0u) &&
          (f[6] >= 0x08000000u) && (f[6] < 0x08200000u)) {
        return f;
      }
    }
  }

  /* 兜底：老办法（xPSR 的 T 位 + IPSR 非 0 + PC 在 flash） */
  for (uint32_t i = 0u; i < 64u; ++i) {
    uint32_t *f   = sp + i;
    uint32_t  pc  = f[6];
    uint32_t  psr = f[7];

    if (((psr & 0x01000000u) != 0u) && ((psr & 0x1FFu) != 0u) &&
        (pc >= 0x08000000u) && (pc < 0x08200000u)) {
      return f;
    }
  }
  return 0;
}

/* 不依赖 SysTick 的忙等（HardFault 时 SysTick 可能已经停了） */
static void App_FaultDelay(volatile uint32_t loops)
{
  while (loops-- != 0u) {
    __NOP();
  }
}

/* HardFault 指示灯：三短一长，循环 —— 与 App_LedTask() 的三种闪法（快闪/1 Hz 慢闪/短脉冲）
   都不同，不接调试器一眼就能认出"进 HardFault 了"。默认停在这里不复位（方便调试器看调用栈、
   也方便 tools/status.ps1 读 g_fault）；SYNTH_PORT_FAULT_RESET = 1 时改为记录后软复位。 */
static void App_FaultBlinkForever(void)
{
  App_LedInit();                       /* 故障可能发生在 App_LedInit() 之前，这里自己配一次 PE3 */

  for (;;) {
    for (int i = 0; i < 3; ++i) {
      HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_SET);
      App_FaultDelay(0x30000u);
      HAL_GPIO_WritePin(GPIOE, GPIO_PIN_3, GPIO_PIN_RESET);
      App_FaultDelay(0x30000u);
    }
    App_FaultDelay(0x180000u);         /* 三短之后停一段长的，再重复 */
  }
}

/* used + externally_visible：本函数只被上面的 naked 汇编按*符号名*调用，编译器看不见引用关系。
   没有这两个属性时 -flto 会把它当死代码删掉（链接报 undefined reference to 'App_HardFaultRecordC'）。 */
__attribute__((used, externally_visible))
void App_HardFaultRecordC(uint32_t exc_return, uint32_t msp, uint32_t psp)
{
  uint32_t *frame;

  /* 兜底：万一拿到的不是 EXC_RETURN（例如以后序言变了），当作"不知道"，一律从栈上找 */
  if ((exc_return & 0xFFFFFFE0u) != 0xFFFFFFE0u) { exc_return = 0u; }

  if ((exc_return != 0u) && ((exc_return & 0x4u) != 0u)) {
    frame = (uint32_t *)psp;                        /* 线程模式 + PSP：PSP 没被动过 → 精确 */
  } else {
    frame = App_FindFaultFrame((uint32_t *)msp);    /* 其它情况：从 MSP 往上找栈帧 */
  }

  ++g_fault.count;
  g_fault.cfsr       = SCB->CFSR;
  g_fault.hfsr       = SCB->HFSR;
  g_fault.mmfar      = SCB->MMFAR;
  g_fault.bfar       = SCB->BFAR;
  g_fault.icsr       = SCB->ICSR;      /* 此刻 VECTACTIVE 恒为 3；出事时在哪个异常看 xpsr 低 9 位 */
  g_fault.exc_return = exc_return;
  g_fault.msp        = msp;
  g_fault.psp        = psp;

  if (frame != 0) {
    g_fault.sp          = (uint32_t)frame;
    g_fault.r0          = frame[0];
    g_fault.r1          = frame[1];
    g_fault.r2          = frame[2];
    g_fault.r3          = frame[3];
    g_fault.r12         = frame[4];
    g_fault.lr          = frame[5];
    g_fault.pc          = frame[6];
    g_fault.xpsr        = frame[7];     /* 低 9 位 = 出事时正在执行的异常号（0 = 线程模式） */
    g_fault.frame_valid = 1u;
  } else {
    g_fault.frame_valid = 0u;           /* 没找到栈帧：至少还有 CFSR/HFSR/MMFAR/BFAR 可判 */
  }

  g_fault.magic = APP_FAULT_MAGIC;      /* 最后写：读到 magic 就说明前面都写完了 */

  /* 串口直接打出来（阻塞写：这是"发完就不返回"的场景，环形缓冲里的东西不会再有机会发出去）。
     注意这里只写 UART 寄存器，不依赖 SysTick/HAL 状态，进 HardFault 也能用。
     发布构建（-DSYNTH_RELEASE=ON → SYNTH_PORT_DEBUG_UART=0）时整段去掉：现场仍在 g_fault 里，
     用 tools\status.ps1 读得到。 */
#if SYNTH_PORT_DEBUG_UART
  {
    char line[256];
    const int n = snprintf(line, sizeof(line),
        "\r\n[HARDFAULT] count=%lu CFSR=0x%08lX HFSR=0x%08lX BFAR=0x%08lX MMFAR=0x%08lX "
        "EXC_RET=0x%08lX frame_valid=%lu PC=0x%08lX LR=0x%08lX xPSR=0x%08lX\r\n",
        (unsigned long)g_fault.count, (unsigned long)g_fault.cfsr, (unsigned long)g_fault.hfsr,
        (unsigned long)g_fault.bfar, (unsigned long)g_fault.mmfar, (unsigned long)g_fault.exc_return,
        (unsigned long)g_fault.frame_valid, (unsigned long)g_fault.pc, (unsigned long)g_fault.lr,
        (unsigned long)g_fault.xpsr);
    if (n > 0) {
      dbg_write_blocking(line, ((uint32_t)n < sizeof(line)) ? (uint32_t)n : (uint32_t)(sizeof(line) - 1u));
    }
  }
#endif

#if SYNTH_PORT_FAULT_RESET
  /* 现场已写进 g_fault（.ram_section，复位不丢）→ 软复位，板子自动恢复发声 */
  NVIC_SystemReset();
#endif
  App_FaultBlinkForever();              /* 不返回 */
}

/* 让"只给外部工具看的"计数器活下来。
   tools/status.ps1 用 arm-none-eabi-nm 按名字找 g_synth_* 这些符号再 dump 内存；但发布构建里
   日志代码被裁掉后，这些计数器在 C 代码里可能一个读者都没有，链接的 --gc-sections（LTO 下更
   激进）会把它们整段回收，符号就没了（现象：status.ps1 报 "ELF 里找不到符号 g_synth_audio_xruns"）。
   这里读一遍塞进 volatile 变量 —— 有真实引用，段就不会被回收，开销只有一次（不是热路径）。
   新增"外部工具要看"的计数器时，加到这个列表里。 */
static void App_KeepObservablesAlive(void)
{
  volatile uint32_t sink = 0u;
  sink ^= g_synth_audio_xruns;
  sink ^= g_synth_audio_underruns;
  sink ^= g_synth_audio_isr_depth;
  sink ^= g_synth_midi_rx_bytes;
  sink ^= g_synth_usb_mount_count;
  sink ^= g_synth_usb_umount_count;
  sink ^= g_synth_usb_irq_count;
  sink ^= g_synth_usb_isr_depth;
  sink ^= g_synth_systick_ticks;
  sink ^= g_synth_usb_clk_rc;
  (void)sink;
}

/* SD 卡初始化：参数与 CubeMX 生成的 MX_SDMMC1_SD_Init()（Core/Src/sdmmc.c:41-50）一致，
   但**允许失败**——没插卡时 HAL_SD_Init() 必定失败，生成代码会 Error_Handler()、整块板停在
   while(1)（USB 不枚举、没声音）。返回 0 = 初始化成功。
   总线用 4-bit（CubeMX 的默认值）：DAT3 交回 SDMMC 外设，速度也够（FATFS/Target/bsp_driver_sd.c
   里 BSP_SD_Init() 之后还会调 HAL_SD_ConfigWideBusOperation(SDMMC_BUS_WIDE_4B)）。 */
#if SYNTH_PORT_SD_ENABLE
static int App_SdInit(void)
{
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv = 1;      /* 数据阶段 48MHz/(2*1) = 24 MHz（≤25 MHz，符合默认速率上限）。
                                  注意别写 0：HAL 在 HAL_SD_ConfigWideBusOperation() 里对
                                  "ClockDiv >= sdmmc_clk/(2*25MHz)"（48 MHz 时 = 0）判真，于是把
                                  CLKDIV 写成 0 = 分频旁路（48 MHz 直接怼卡），实测表现为
                                  HAL 返回 RX_OVERRUN(32) → f_mount 报 FR_DISK_ERR(1)。 */
  /* SDMMC 的 IDMA 完成靠中断通知（FatFs 的读写已改走 DMA，见 FATFS/Target/sd_diskio.c）：
     优先级 6 —— 低于音频 DMA(0) 与 USB(5)。IDMA 自己搬数据，CPU 晚几百微秒响应只是慢一点，
     不会像轮询模式那样被音频中断抢占到 FIFO 溢出。 */
  HAL_NVIC_SetPriority(SDMMC1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(SDMMC1_IRQn);

  return (HAL_SD_Init(&hsd1) == HAL_OK) ? 0 : -1;
}
#endif

/* SDMMC1 中断：HAL_SD_IRQHandler() 在里面调 BSP_SD_ReadCpltCallback()/WriteCpltCallback()，
   FatFs 的 SD 读写（FATFS/Target/sd_diskio.c 里的 DMA 版本）就在等这个回调清忙标志。
   注意：CubeMX 的 Core/Src/sdmmc.c 不生成这个函数；以后若在 CubeMX 里勾了 SDMMC1 的 NVIC，
   它会在 Core/Src/stm32h7xx_it.c 生成一份，届时删掉那一份（否则重复定义编译不过）。 */
void SDMMC1_IRQHandler(void)
{
  HAL_SD_IRQHandler(&hsd1);
}

#if SYNTH_PORT_SD_ENABLE   /* App_SdTryPowerMode 依赖上面的 App_SdInit()，只在 SD 编进来时才编译 */
/* 把 PD4（MicroSD_SW）按指定状态配好再试一次初始化 —— 曾经用来验证"PD4 是不是卡座电源"，
   结论是三种状态（输出低 / 输出高 / 输入上拉）全都初始化失败，PD4 与 SD 无关。
   保留这段代码是为了以后换板子时能一键复现这个实验：把 main() 第 9 步换成
   for (mode = 1; mode <= 3; ++mode) { if (App_SdTryPowerMode(mode) == 0) break; } 即可。 */
static int App_SdTryPowerMode(uint32_t mode) __attribute__((unused));
static int App_SdTryPowerMode(uint32_t mode)
{
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = MicroSD_SW_Pin;                     /* PD4 */
  if (mode == 3u) {
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
  } else {
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
  }
  HAL_GPIO_Init(MicroSD_SW_GPIO_Port, &gpio);
  if (mode != 3u) {
    HAL_GPIO_WritePin(MicroSD_SW_GPIO_Port, MicroSD_SW_Pin,
                      (mode == 1u) ? GPIO_PIN_RESET : GPIO_PIN_SET);
  }
  HAL_Delay(5);                                  /* 等卡座电源稳定 */
  return App_SdInit();
}
#endif

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 20;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* 应用入口。
   ⚠ CubeMX 的 Project Manager 里勾了 "Do not generate the main()"，所以这个 main() 只存在于
     这里：CubeMX 重新生成 main.c 时会跳过 main()，但文件的其余部分（生成区）会照 .ioc 重写，
     USER CODE 区原样保留。
     代价：以后在 CubeMX 里新加外设（比如再开一个 UART），要自己把新的 MX_xxx_Init() 补进这里。 */
int main(void)
{
  synth_config_t cfg;

  /* 0) 复位计数与复位原因：放在最前 —— 后面任何一步崩掉（HardFault 钩子会 NVIC_SystemReset）
        都能从 g_app_status.boot_count 看出来是「复位循环」，而不是「USB 没枚举」 */
  App_RecordBoot();
  App_KeepObservablesAlive();   /* 保住 status.ps1 要读的计数器符号（--gc-sections/LTO 会回收） */

  /* 1) 内存与时钟。MPU_Config() 必须最先：Region0 把 0x60000000–0xDFFFFFFF 判成 NO_ACCESS，
        而 0x90000000（QSPI 映射窗口）是靠 Region2 放行的 —— 任何对它的访问都必须在
        MPU_Config() 之后；Region2 就在 MPU_Config() 里（CubeMX 的 CORTEX_M7 → MPU 配的）。 */
  MPU_Config();
  SCB_EnableICache();
  HAL_Init();
  SystemClock_Config();

  /* 1.5) 调试串口：USART1（CubeMX 生成，PB14 = TX、PB15 = RX、115200 8N1、无 DMA）。
         先调生成的 MX_USART1_UART_Init()（它就是 HAL_UART_Init + MspInit：引脚/时钟/波特率），
         再调 dbg_init() 只做环形缓冲清零 + 使能 USART1 中断。
         打开开关：Synth/port/synth_port_config.h 的 SYNTH_PORT_DEBUG_UART（默认已开）。 */
  MX_USART1_UART_Init();
  dbg_init(SYNTH_PORT_DEBUG_BAUD);
  dbg_log("boot", "==== STM32synthesizer boot ====");
  dbg_log("boot", "sysclk=%lu Hz hclk=%lu Hz pclk2=%lu Hz",
          (unsigned long)SystemCoreClock, (unsigned long)HAL_RCC_GetHCLKFreq(),
          (unsigned long)HAL_RCC_GetPCLK2Freq());
  /* 复位原因逐位展开太占 flash（调试版只剩几百字节），要看请用 tools\status.ps1
     的 reset_flags 字段（那里会解码成 POR/PIN/SFT/CPU/D1/BOR）。 */
  dbg_log("boot", "boot_count=%lu reset_flags=0x%08lX",
          (unsigned long)g_app_status.boot_count, (unsigned long)g_app_status.reset_flags);
  if (g_fault.count != 0u) {
    dbg_log("boot", "上次运行进过 HardFault（count=%lu）；g_fault 里还留着现场", (unsigned long)g_fault.count);
  }

  /* 2) .dtcm_bss 是 NOLOAD，启动代码不清零 —— 用之前先清零 */
  App_ZeroDtcmBss();

  /* 3) 外设。顺序有讲究：
        * MX_DMA_Init() 要早于 MX_SAI3_Init()（SAI 初始化时要把 DMA1_Stream0 链给 SAI3_B）；
        * MX_SDMMC1_SD_Init() 要早于第一次 f_mount()（在 synth_port_sd_mount() 里）；
        * MX_FATFS_Init() 只是登记驱动（FATFS/App/ff_gen_drv.c:39），顺序无所谓。

     ⚠ SDMMC 只在「开了 SD 功能 且 上电时插着卡」时才初始化：CubeMX 生成的
        MX_SDMMC1_SD_Init() 在 HAL_SD_Init() 失败时直接 Error_Handler()（Core/Src/sdmmc.c:47-50），
        而没插卡时 HAL_SD_Init() 必定失败 —— 那会把整块板子停在 Error_Handler 的 while(1) 里
        （USB 不枚举、没声音，看着像死机）。没插卡就整段跳过，其余功能照常。
        卡要上电前插好；以后要做热插拔，就在这里改成"检测到卡再初始化 + f_mount"的懒加载。 */
  MX_DMA_Init();
  MX_GPIO_Init();
  App_LedInit();                        /* PE3 心跳灯（诊断用，见 App_LedTask） */
  App_KeyInit();                        /* PC13 用户键（转发给 RAM overlay 模块） */
  MX_QUADSPI_Init();                    /* 配 QUADSPI 参数；进内存映射在下面第 5 步 */
  /* SD 卡初始化在下面第 8 步用 App_SdInit() 做（允许失败、失败只记 -3）：
     生成的 MX_SDMMC1_SD_Init() 里 HAL_SD_Init() 失败会直接 Error_Handler，
     没插卡时会把整块板子停在 while(1) 里（USB 不枚举、没声音）。 */
  MX_FATFS_Init();

  /* 4) 音频通路：SAI3 + MCLK 补丁 + GPIO 速度 + 时钟自检 */
  App_InitAudioPath();
  dbg_log("sai", "audio path: ck=%lu Hz mckdiv=%lu fault=0x%08lX",
          (unsigned long)g_app_status.audio_ck_hz, (unsigned long)g_app_status.sai_mckdiv,
          (unsigned long)g_app_status.fault);

  /* 5) 外部 QSPI flash 进内存映射。采样库（sample_storage_qspi.c 直读 0x90000000）依赖它；
        失败只记故障位、不阻塞启动 —— 引擎、SD 采样、MIDI 都还能用。 */
  if (App_QspiEnterMemoryMapped() != 0) {
    App_Fault(APP_FAULT_QSPI_MM);
  }
  dbg_log("qspi", "memory-mapped %s, JEDEC id=0x%06lX",
          ((g_app_status.fault & APP_FAULT_QSPI_MM) == 0u) ? "OK" : "FAILED",
          (unsigned long)g_app_status.qspi_id);

  /* 5b) 数据通路自检（默认关，见 SYNTH_PORT_QSPI_WRITE_TEST）：擦最后一个 4 KB 扇区 →
         写 16 字节图案 → 从 0x90000000 读回比对。只验证过"读 ID"是不够的 ——
         Synth/port/sample_storage_qspi.c 是**零拷贝直读映射窗口**的，这一步过了才能放心把
         采样/音色库放进 QSPI。0 = 全对，负数 = 第几步失败（见字段注释）。
         该扇区（0x7FF000）已被自检占用，采样库别放那儿。 */
#if SYNTH_PORT_QSPI_WRITE_TEST
  if ((g_app_status.fault & APP_FAULT_QSPI_MM) == 0u) {
    g_app_status.qspi_wtest = (uint32_t)App_QspiWriteTest();
    dbg_log("qspi", "write/read-back test: %ld (read=0x%08lX, expect 0x514E5953)",
            (long)(int32_t)g_app_status.qspi_wtest, (unsigned long)g_app_status.qspi_wtest_rd);
  } else {
    g_app_status.qspi_wtest = 0x7FFFFFFFu;   /* 没进内存映射，跳过自检 */
  }
#else
  g_app_status.qspi_wtest = 0x7FFFFFFFu;     /* 自检未开启（已于 2026-10-19 验证通过） */
#endif

  /* 6) 引擎与端口层。synth_port_init() 里做的事：开 D-Cache、FPU 置 flush-to-zero、
        使能 DWT 周期计数、选 USB 内核时钟、装平台钩子（见 Synth/port/platform_stm32.c）。 */
  synth_port_init();

  cfg.sample_rate       = SYNTH_SR_DEFAULT;        /* 48000（Synth/include/synth/patch.h:11） */
  /* 声部上限：按实测定的（2026-10-19，出厂音色 1，16 声部同时按下的 cpu_permille）：
        -O2 原始 2121‰ → 控制率 8→32 + 噪声/子振按需 1450‰ → -O3 -ffast-math 1311‰。
     即 16 声部要 1.3 倍实时；单声部约 8.6% 实时预算，实时上限约 11 声部。
     取 10 留 15~20% 余量给效果链和 FM 音色（FM 单声部成本是模拟音色的 3 倍多）。
     想更多声部：先做 DSP 优化（正弦表插值、把引擎放进 ITCM），再把这里调大。 */
  cfg.max_voices        = 10u;
  cfg.factory_bank      = synth_factory_bank;      /* patch.h:100 内置出厂音色表 */
  cfg.factory_bank_size = synth_factory_bank_size; /* patch.h:101 */

  g_app_status.synth_init = synth_init(&cfg);
  if (g_app_status.synth_init != 0) {
    App_Fault(APP_FAULT_SYNTH_INIT);
  }
  dbg_log("init", "synth_init=%ld (0=OK), patch=%lu", (long)g_app_status.synth_init,
          (unsigned long)synth_get_patch_index());

  /* 7) USB-MIDI 放到 SD 之后（见步骤 8 末尾）：QSPI 采样库导入要擦写好几十秒，
     这期间主循环还没开始跑（tud_task 不会被调用），如果 USB 已经拉起来，
     主机会看到"枚举了但不响应"的设备并可能报错。tusb_init() 之前 OTG 核不上拉。 */

  /* 8) SD 卡：只有 Synth/port/synth_port_config.h 里 SYNTH_PORT_SD_ENABLE = 1 时才编进来。
     为什么不用 MX_SDMMC1_SD_Init()：CubeMX 生成的 Core/Src/sdmmc.c 在 HAL_SD_Init() 失败时直接
     Error_Handler()（整块板停在 while(1)：USB 不枚举、没声音）。没插卡时 HAL_SD_Init() 必定失败，
     所以这里自己配同样的参数但**允许失败** —— 没卡就当作"没有 SD"，合成器照常工作。

     两个已经排除/确认的硬件事实（都记在 docs/handoff.md）：
       * PD4（CubeMX 里叫 MicroSD_SW）不是卡检测也不是卡座电源：输出低/输出高/输入上拉三种状态
         各试一次初始化，全部失败（WeAct 官方 04-SD_Test 例程也没有任何检测脚）。
       * **DAT3(PC11) 是关键**：实测插卡时 PC11 = 0、拔卡时 = 1（其余 D0/D1/D2/CK/CMD 都不变）。
         SD 模式下 DAT3 同时就是 SPI 模式的 CS —— 卡上电时 CS 为低，卡就进 SPI 模式，之后用 SD
         原生命令（CMD8 等）问它一声不吭 ⇒ HAL 报 ErrorCode=4「CMD 响应超时」，与现象完全吻合。
     2026-10-19 结论更新：**旧卡就是坏/挑设备的**——换 32 GB 卡后 HAL_SD_Init() 完全成功
     （ErrorCode = 0、CardType = SDHC、61,132,800 块 × 512 B）。所以 DAT3(PC11) 不再人为顶高：
     恢复 4-bit 总线、DAT3 交回 SDMMC 外设。之前把它当普通 GPIO 输出高会和 4-bit 传输抢同一条线，
     实测表现就是 HAL 返回 RX_OVERRUN(32) → f_mount() 报 FR_DISK_ERR(1)。
     sd_dat3 字段保留但恒为 0：0 = DAT3 已交回外设（4-bit 正常）；1 = 历史值（曾用 GPIO 顶高）。 */
#if SYNTH_PORT_SD_ENABLE
  g_app_status.sd_dat3 = 0u;
  if (App_SdInit() != 0) {
    g_app_status.sd_err = hsd1.ErrorCode;   /* 卡没起来：把 HAL 的错误位留下来给 status.ps1 看 */
    g_app_status.sd_mount = -3;        /* -3 = 卡初始化失败（没插卡 / 卡座硬件问题）：不置故障位 */
  } else {
    g_app_status.sd_mount = synth_port_sd_mount();
    g_app_status.sd_err = hsd1.ErrorCode;   /* 挂载失败时这里通常是 32(RX_OVERRUN)/8(DATA_TIMEOUT) 等 */
    if (g_app_status.sd_mount != 0) {
      App_Fault(APP_FAULT_SD_MOUNT);   /* 卡能初始化却挂不上文件系统 → 值得报警（格式/接触问题） */
    } else {
      /* 挂载成功 → 跑一次读写自检，并把 15 个内置音色导出成 /synth/NNN.syn 模板
         （已存在的文件不覆盖，所以卡上改过的音色不会被开机冲掉）。结果存进 g_app_status，
         用 tools/status.ps1 读；位含义见 Synth/port/patch_storage_sd.c 顶部注释。 */
      g_app_status.sd_selftest = synth_port_sd_selftest();
      g_app_status.sd_exported = synth_port_sd_exported();
      /* QSPI 采样库导入（第三档）：卡上 /synth/samples.bin 的 serial 与板上库不同就导入。
         必须赶在起音频之前（导入要擦写几十秒）；serial 相同则立刻返回。 */
      g_app_status.lib_import = (uint32_t)synth_port_lib_import_from_card();
      dbg_log("lib", "import=%ld count=%lu serial=0x%lx",
              (long)(int32_t)g_app_status.lib_import,
              (unsigned long)synth_port_lib_count(), (unsigned long)synth_port_lib_serial());
      /* 开机恢复"每个通道用哪个音色号"（/synth/state.cfg），并立刻让 patch_service 把卡上
         对应的 .syn 取进来 —— 必须赶在音频 DMA 启动之前做完（见下面步骤 7 的说明）。 */
      g_app_status.state_load = (uint32_t)synth_port_state_load();
      synth_port_patch_service();
      /* 覆盖自检（默认关，见 SYNTH_PORT_SD_WRITE_TEST）：证明"卡上的内容说了算"
         （写改过的 000.syn → 读回确认 → 恢复原件）。位含义见 Synth/port/patch_storage_sd.c；
         开启时期望 0x0F。 */
#if SYNTH_PORT_SD_WRITE_TEST
      g_app_status.sd_wtest = synth_port_sd_write_test();
#else
      g_app_status.sd_wtest = 0x80000000u;   /* 自检未开启（已于 2026-10-19 验证通过 = 0x0F） */
#endif
      dbg_log("sd", "mounted (dat3=%lu), selftest=0x%lx exported=%lu wtest=0x%lx",
              (unsigned long)g_app_status.sd_dat3,
              (unsigned long)g_app_status.sd_selftest, (unsigned long)g_app_status.sd_exported,
              (unsigned long)g_app_status.sd_wtest);
    }
  }
#else
  g_app_status.sd_mount = -1;
#endif

  /* 8b) QSPI 采样库状态：不管有没有 SD 都读一次（库可能是上次导入的，也可能被自检覆盖过）。
         用 tools/status.ps1 看 lib_count / lib_serial / lib_import。 */
  g_app_status.lib_count  = synth_port_lib_count();
  g_app_status.lib_serial = synth_port_lib_serial();
#if SYNTH_PORT_LIB_SELFTEST_ON_BOOT
  /* 没有 PC 打包文件时用它验证整条链路（见 synth_port_config.h 的说明）：造小库 → 写 QSPI
     → 重新枚举，发 PC 120 应该听到 93.75 Hz 的正弦。会覆盖真库，验证完记得关掉。 */
  if (g_app_status.lib_count == 0u) {
      const int lib_rc = synth_port_lib_selftest();
      dbg_log("lib", "selftest rc=%d count=%lu serial=0x%lx", lib_rc,
              (unsigned long)synth_port_lib_count(), (unsigned long)synth_port_lib_serial());
      g_app_status.lib_count  = synth_port_lib_count();
      g_app_status.lib_serial = synth_port_lib_serial();
  }
#endif

  /* 8d) RAM overlay 模块（TFT UI 等）：卡上 /synth/ui.bin 的 build_id/CRC 与板上不同就写进
         QSPI 0x7F0000，然后校验并整段拷到 AXI SRAM 顶部执行。模块是可选的：失败只记录，
         不影响音频/MIDI（module_state 见 Synth/port/module.h）。 */
  g_app_status.module_import = (uint32_t)synth_port_module_import_from_card();
  g_app_status.module_state  = -6;   /* -6 = 还没走到（万一 init 崩了，status 不会误报 0 = 已加载） */
  g_app_status.module_state  = (int32_t)synth_module_init();
  g_app_status.module_crc    = synth_module_crc();
  dbg_log("mod", "import=%ld state=%ld crc=0x%lx", (long)(int32_t)g_app_status.module_import,
          (long)g_app_status.module_state, (unsigned long)g_app_status.module_crc);

  /* 8c) USB-MIDI（TinyUSB，设备模式 FS）：放在库导入/SD 之后，避免主机在导入期间
         看到一个"枚举成功但不响应"的设备。 */
  g_app_status.usb_init = synth_port_usb_init();
  if (g_app_status.usb_init != 0) {
    App_Fault(APP_FAULT_USB_INIT);
  }
  dbg_log("usb", "tusb_init=%ld (0=OK), usb_clk=%lu Hz", (long)g_app_status.usb_init,
          (unsigned long)App_UsbKernelClockHz());
  dbg_log("boot", "==== 进入主循环（等主机枚举 + MIDI 输入）====");

  /* 9) 起音频 DMA —— **故意放在 SD 之后**：卡上的音色映射（state.cfg + /synth/NNN.syn）要在
        出声之前全部就位；否则开机读卡会占住主循环，预渲染环喂不上（实测 audio_underruns +10）。
        先渲染两个半缓冲再启动，之后由 SAI3_B 的 DMA 半传输/传输完成中断续渲染。 */
  g_app_status.audio_start = synth_port_audio_start();
  if (g_app_status.audio_start != 0) {
    App_Fault(APP_FAULT_AUDIO_START);
  }
  dbg_log("sai", "audio_start=%ld (0=OK)", (long)g_app_status.audio_start);

  /* 10) 时钟自检结果（调试器里看 g_app_status）：
          sysclk 480 MHz / hclk 240 MHz / usb 48 MHz / audio 24.576 MHz / MCKDIV 2 / qspi_id 0xEF4017 */
  g_app_status.sysclk_hz = (uint32_t)SystemCoreClock;
  g_app_status.hclk_hz   = HAL_RCC_GetHCLKFreq();
  g_app_status.usb_ck_hz  = App_UsbKernelClockHz();
  g_app_status.usb_clk_rc = g_synth_usb_clk_rc;

  /* 11) 主循环：三件事都必须在非中断上下文做
          * tud_task()        —— USB 收发；MIDI 包在这里被解析并交给 synth_midi_in()
          * patch_service()   —— 把 ISR 里排队的 patch 请求落地（要读 SD 文件）
          * synth_service()   —— 引擎的非实时事务（波表/采样切换、patch 生效） */
  while (1)
  {
    /* 0) 先喂音频：环缓冲有空位就提前渲染一块（渲染在主循环，不在优先级 0 的中断里）。
          这是"复音一多就断续/卡死"的根治措施：中断只做 memcpy，CPU 再忙也不会拖死 USB。 */
    synth_port_audio_pump();

    /* main_stage 标记"卡在哪一步"：halt 时看它就知道主循环停在哪里。配合 systick_ticks 判死活：
         systick 不涨       → CPU 停在优先级 0 的中断里（SysTick 优先级 15 被屏蔽）或 Error_Handler 里
         systick 涨但 loop 不涨 → 主循环卡在 main_stage 那一步的阻塞调用里 */
    g_app_status.main_stage = 1u;
    synth_port_usb_task();
    g_app_status.main_stage = 2u;
    (void)synth_port_lib_service();  /* QSPI 库优先：主 PC 命中库里的乐器就零拷贝挂载 */
    synth_port_patch_service();
    synth_port_state_service();   /* 通道音色映射变了 → 静默 2 s 后写回 /synth/state.cfg */
    g_app_status.main_stage = 3u;
    synth_service();
    synth_module_tick();          /* RAM overlay 模块（TFT UI）：100 Hz 节流，模块自己画屏 */
    App_KeyTask();                /* PC13 消抖 → synth_module_key() */
    g_app_status.main_stage = 0u;

    /* 模块状态（module_ticks 不涨 = 模块自己的 tick 卡住了；probe 由模块定义）。
       state/crc 也要每圈刷新：USB 上传新镜像后会卸载+重新加载，开机时那一次的值就过期了。 */
    g_app_status.module_state    = (int32_t)synth_module_state();
    g_app_status.module_crc      = synth_module_crc();
    g_app_status.module_ticks    = synth_module_ticks();
    g_app_status.module_probe[0] = g_module_probe[0];
    g_app_status.module_probe[1] = g_module_probe[1];
    g_app_status.up_state        = synth_port_module_upload_state();

    /* 心跳与诊断：LED 编码状态（见 App_LedTask），USB 寄存器快照只在未枚举时刷新。
       loop_count 不涨 = 主循环卡死（而不是"USB 谈不拢"）。 */
    ++g_app_status.loop_count;
    App_LedTask();

    /* 存活探针（都是 volatile 全局，由中断更新） */
    g_app_status.systick_ticks   = g_synth_systick_ticks;
    g_app_status.usb_isr_depth   = g_synth_usb_isr_depth;
    g_app_status.audio_isr_depth = g_synth_audio_isr_depth;

    /* 负载指标（读引擎内部统计；用来验证"复音多"时是否还有余量）+ MIDI 控制面计数 */
    {
      synth_stats_t st;
      synth_get_stats(&st);
      g_app_status.cpu_permille       = synth_cpu_load_permille();
      g_app_status.render_peak_cycles = st.render_peak_cycles;
      g_app_status.nrpn_used          = st.nrpn_used;
      g_app_status.nrpn_unmapped      = st.nrpn_unmapped;
      g_app_status.last_nrpn          = st.last_nrpn;
    }
    g_app_status.audio_underruns = g_synth_audio_underruns;
    g_app_status.state_saves     = synth_port_state_save_count();

    /* 多音色映射：16 个通道的音色号打包成 4 个字（每通道 4 bit，15 = 未知/越界） */
    for (uint32_t w = 0; w < 4u; ++w) {
      uint32_t packed = 0u;
      for (uint32_t n = 0; n < 4u; ++n) {
        const uint16_t p = synth_get_channel_patch((uint8_t)(w * 4u + n));
        packed |= ((p > 14u) ? 15u : (uint32_t)p) << (4u * n);
      }
      g_app_status.chan_patch[w] = packed;
    }

#if SYNTH_PORT_USB_ENABLE
    /* USB 诊断（调试器里看 g_app_status）：
         usb_mounted = 0 → 主机没枚举成功，问题在固件/硬件侧（PHY 供电、时钟、AF、线、口）
         usb_mounted = 1 而 Windows 里仍看不到 → 问题在主机侧（驱动/查看位置/线材质量）
       第二轮：usb_irq_count / usb_gotgctl / usb_dsts / usb_dctl 用来区分
         「核没中断」「中断了但没谈成」「时钟或 PHY 没起来」 */
    g_app_status.usb_mounted      = tud_mounted() ? 1u : 0u;
    if (g_app_status.usb_mounted == 0u) {
      App_SnapshotUsb();            /* 只在没枚举成功时刷新：正常运行时省掉每圈的寄存器读 */
    }
    g_app_status.midi_rx_bytes    = g_synth_midi_rx_bytes;
    g_app_status.usb_mount_count  = g_synth_usb_mount_count;
    g_app_status.usb_umount_count = g_synth_usb_umount_count;
#endif

    /* ---- 1 Hz 统计行 + 卡死检测 -----------------------------------------------
       统计行是判断「音停不下来 / 卡住没声音」的主要依据：
         voices    = 引擎当前发声声部数（松开所有键后应回到 0；一直不降 = note-off 没到，或引擎没释放）
         on/steal  = 收到多少 note-on / 偷音次数（steal 涨 = 16 声部不够）
         midi/drop = USB 收到的 MIDI 字节数 / 引擎环形缓冲丢弃字节数（drop 涨 = 引擎侧来不及处理）
         boom      = 单块渲染最大 CPU 周期（480 MHz 下 1 ms = 480000 周期，接近就危险）
         xrun      = SAI DMA 出错次数（>0 就是声音断续的来源）
         snd       = 主循环当前阶段（1 usb_task / 2 patch_service / 3 synth_service）
       卡死检测：主循环 200 ms 没推进就打一行 STALL（带阶段号和两个中断深度）。
       发布构建（SYNTH_PORT_DEBUG_UART=0）里这整块不编：它只为日志存在。 */
#if SYNTH_PORT_DEBUG_UART
    {
      static uint32_t s_stat_ms = 0u;
      static uint32_t s_loop_prev = 0u;
      const uint32_t now = HAL_GetTick();

      /* 卡死检测留给 tools\status.ps1（loop_count 不涨就是卡了，还能看到 stage/中断深度），
         这里只保留每秒一行统计 —— 调试版 flash 只剩一百多字节，字符串省着用。 */
      if ((now - s_stat_ms) >= 1000u) {
        synth_stats_t st;
        const uint32_t cpu = synth_cpu_load_permille();
        synth_get_stats(&st);
        dbg_log("stat", "loop=%lu voices=%u/%u midi=%lu drop=%lu cc=%lu cpu=%lu.%lu%% xrun=%lu",
                (unsigned long)(g_app_status.loop_count - s_loop_prev),
                (unsigned)st.active_voices, (unsigned)st.peak_voices,
                (unsigned long)st.midi_bytes, (unsigned long)st.midi_dropped,
                (unsigned long)st.unknown_cc,
                (unsigned long)(cpu / 10u), (unsigned long)(cpu % 10u),
                (unsigned long)g_synth_audio_xruns);
        s_stat_ms = now;
        s_loop_prev = g_app_status.loop_count;
      }
    }
#endif /* SYNTH_PORT_DEBUG_UART */
  }
}

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x08000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_128KB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_8MB;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  /* 可缓存（2026-10-19 改）：采样零拷贝播放从这块地址取数据，命中 D-Cache 后基本免费；
     非缓存时每次采样点都要真的去 flash（24 MB/s 上限）。只读 + 不可执行，写入只走
     HAL_QSPI 间接模式（不经过这条映射），所以缓存安全 —— 导入完成后清一次 cache 即可
     （synth_port_qspi_cache_invalidate()）。 */
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  g_app_status.fault |= APP_FAULT_HAL;
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
