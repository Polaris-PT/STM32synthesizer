/* QSPI flash 底层原语（详见 qspi_flash.h 的说明）。
   从 Core/Src/main.c 的 App_QspiEnterMemoryMapped() 等函数搬过来的，逻辑没变 ——
   搬家的目的：采样库的导入器（Synth/port/sample_storage_qspi.c）也要擦/写/映射，
   它不能反过来调 main.c 里的 static 函数。 */
#include "synth_port_config.h"
#include "qspi_flash.h"
#include "quadspi.h"          /* hqspi */
#include "stm32h7xx_hal.h"

#ifndef SYNTH_QSPI_DUMMY_CYCLES
#define SYNTH_QSPI_DUMMY_CYCLES 4u
#endif

static uint32_t s_qspi_id;
static int      s_mmap = 0;   /* 1 = 当前处于内存映射模式 */

/* 所有命令的公共字段：单线指令、无地址/无数据，下面按需覆盖 */
static void cmd_init(QSPI_CommandTypeDef *cmd) {
    __builtin_memset(cmd, 0, sizeof(*cmd));
    cmd->InstructionMode    = QSPI_INSTRUCTION_1_LINE;
    cmd->AddressMode        = QSPI_ADDRESS_NONE;
    cmd->AddressSize        = QSPI_ADDRESS_24_BITS;
    cmd->AlternateByteMode  = QSPI_ALTERNATE_BYTES_NONE;
    cmd->AlternateBytes     = 0x00u;
    cmd->AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    cmd->DataMode           = QSPI_DATA_NONE;
    cmd->NbData             = 0u;
    cmd->DummyCycles        = 0u;
    cmd->DdrMode            = QSPI_DDR_MODE_DISABLE;
    cmd->DdrHoldHalfCycle   = QSPI_DDR_HHC_ANALOG_DELAY;
    cmd->SIOOMode           = QSPI_SIOO_INST_EVERY_CMD;
}

/* 退出内存映射（写之前必须）：HAL 在映射模式下会拒绝所有 HAL_QSPI_* 命令。
   不判 s_mmap —— main.c 也可能自己进过映射（App_QspiEnterMemoryMapped），
   HAL_QSPI_Abort() 在空闲态下是安全的空操作。 */
static void exit_mmap(void) {
    (void)HAL_QSPI_Abort(&hqspi);
    s_mmap = 0;
}

static int mmap_config(void);   /* 只"重新进映射"，实现见 enter_mmap 之后 */

int synth_port_qspi_enter_mmap(void) {
    QSPI_CommandTypeDef cmd;
    uint8_t  id[3] = {0u, 0u, 0u};
    uint8_t  sr2   = 0u;
    uint8_t  sr1   = 0xFFu;
    uint8_t  value = 0u;
    uint32_t guard;

    cmd_init(&cmd);

    /* 1) 软复位：0x66 + 0x99（失败不致命，继续往下试） */
    cmd.Instruction = 0x66u;
    (void)HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE);
    cmd.Instruction = 0x99u;
    (void)HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE);
    HAL_Delay(2u);                              /* 复位期间不收命令（tRST） */

    /* 2) 读 JEDEC ID：W25Q64 = EF 40 17 */
    cmd.Instruction = 0x9Fu;
    cmd.DataMode    = QSPI_DATA_1_LINE;
    cmd.NbData      = 3u;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -1;
    if (HAL_QSPI_Receive(&hqspi, id, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)     return -2;
    s_qspi_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];

    /* 3) 确认 QE（SR2 bit1）：QE=0 时 IO2/IO3 还是 WP#/HOLD#，四线读必然错 */
    cmd.Instruction = 0x35u;                    /* Read Status Register-2 */
    cmd.NbData      = 1u;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -3;
    if (HAL_QSPI_Receive(&hqspi, &sr2, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)  return -4;

    if ((sr2 & 0x02u) == 0u) {
        cmd.Instruction = 0x06u;                /* Write Enable */
        cmd.DataMode    = QSPI_DATA_NONE;
        cmd.NbData      = 0u;
        if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -5;

        value = (uint8_t)(sr2 | 0x02u);
        cmd.Instruction = 0x31u;                /* Write Status Register-2（1 字节） */
        cmd.DataMode    = QSPI_DATA_1_LINE;
        cmd.NbData      = 1u;
        if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -6;
        if (HAL_QSPI_Transmit(&hqspi, &value, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -7;

        cmd.Instruction = 0x05u;                /* Read Status Register-1，等 WIP 清零 */
        cmd.NbData      = 1u;
        for (guard = 0u; guard < 100000u; ++guard) {
            if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -8;
            if (HAL_QSPI_Receive(&hqspi, &sr1, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)  return -9;
            if ((sr1 & 0x01u) == 0u) break;
        }
        if ((sr1 & 0x01u) != 0u) return -10;    /* 超时：flash 没应答（查 CS/时钟/供电） */
    }

    /* 4) 进内存映射（0xEB 四线快读 1-4-4，模式字节 0x00 = 不进连续读） */
    return mmap_config();
}

/* 只"重新进映射"：不碰 flash 设置（不软复位、不读 ID/QE）。
   HAL_QSPI_Abort() 之后控制器回到空闲态、flash 仍在正常模式，发一次 0xEB +
   HAL_QSPI_MemoryMapped 就够 —— 比 enter_mmap()（软复位 + 2 ms 延时 + QE 检查）便宜两个数量级。
   为什么必须显式恢复：0x90000000 上的指针读（采样库 PCM、模块镜像）要求控制器处于映射模式，
   否则 AHB 直接回 BusFault（BFAR 就是那个地址）—— 这是 2026-10-20 模块加载崩溃的根因。 */
static int mmap_config(void) {
    QSPI_CommandTypeDef      cmd;
    QSPI_MemoryMappedTypeDef mm = {0};

    cmd_init(&cmd);
    cmd.Instruction        = 0xEBu;
    cmd.Address            = 0x00000000u;
    cmd.AddressMode        = QSPI_ADDRESS_4_LINES;
    cmd.AddressSize        = QSPI_ADDRESS_24_BITS;
    cmd.AlternateByteMode  = QSPI_ALTERNATE_BYTES_4_LINES;
    cmd.AlternateBytes     = 0x00u;
    cmd.AlternateBytesSize = QSPI_ALTERNATE_BYTES_8_BITS;
    cmd.DummyCycles        = SYNTH_QSPI_DUMMY_CYCLES;
    cmd.DataMode           = QSPI_DATA_4_LINES;
    cmd.NbData             = 0u;

    mm.TimeOutActivation   = QSPI_TIMEOUT_COUNTER_DISABLE;
    mm.TimeOutPeriod       = 0u;

    if (HAL_QSPI_MemoryMapped(&hqspi, &cmd, &mm) != HAL_OK) return -11;
    s_mmap = 1;
    return 0;
}

int synth_port_qspi_mmap_reenter(void) {
    exit_mmap();                 /* 已经在映射模式时也能安全重入（Abort 是空闲态空操作） */
    return mmap_config();
}

int synth_port_qspi_ensure_mmap(void) {
    if (s_mmap != 0) return 0;
    return mmap_config();
}

uint32_t synth_port_qspi_id(void) { return s_qspi_id; }

int synth_port_qspi_write_enable(void) {
    QSPI_CommandTypeDef cmd;
    exit_mmap();
    cmd_init(&cmd);
    cmd.Instruction = 0x06u;
    return (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK) ? 0 : -1;
}

int synth_port_qspi_wait_wip(void) {
    QSPI_CommandTypeDef cmd;
    uint8_t sr1 = 0xFFu;
    uint32_t guard;

    cmd_init(&cmd);
    cmd.Instruction = 0x05u;                    /* Read Status Register-1 */
    cmd.DataMode    = QSPI_DATA_1_LINE;
    cmd.NbData      = 1u;
    for (guard = 0u; guard < 100000u; ++guard) {
        if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -1;
        if (HAL_QSPI_Receive(&hqspi, &sr1, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)  return -1;
        if ((sr1 & 0x01u) == 0u) return 0;
        HAL_Delay(1u);
    }
    return -1;
}

int synth_port_qspi_erase_sector(uint32_t offset) {
    QSPI_CommandTypeDef cmd;
    if (offset >= SYNTH_QSPI_SIZE_BYTES) return -1;
    exit_mmap();
    if (synth_port_qspi_write_enable() != 0) return -2;

    cmd_init(&cmd);
    cmd.Instruction = 0x20u;                    /* Sector Erase（4 KB） */
    cmd.Address     = offset & ~(SYNTH_QSPI_SECTOR_BYTES - 1u);
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -3;
    return synth_port_qspi_wait_wip();
}

int synth_port_qspi_page_program(uint32_t offset, const void *data, uint32_t len) {
    QSPI_CommandTypeDef cmd;
    if (data == NULL || len == 0u || len > SYNTH_QSPI_PAGE_BYTES) return -1;
    if (offset + len > SYNTH_QSPI_SIZE_BYTES) return -1;
    if ((offset / SYNTH_QSPI_PAGE_BYTES) != ((offset + len - 1u) / SYNTH_QSPI_PAGE_BYTES)) return -1;

    exit_mmap();
    if (synth_port_qspi_write_enable() != 0) return -2;

    cmd_init(&cmd);
    cmd.Instruction = 0x02u;                    /* Page Program */
    cmd.Address     = offset;
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    cmd.DataMode    = QSPI_DATA_1_LINE;
    cmd.NbData      = len;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -3;
    if (HAL_QSPI_Transmit(&hqspi, (uint8_t *)data, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) return -4;
    return synth_port_qspi_wait_wip();
}

int synth_port_qspi_read(uint32_t offset, void *buf, uint32_t len) {
    QSPI_CommandTypeDef cmd;
    if (buf == NULL || len == 0u || offset + len > SYNTH_QSPI_SIZE_BYTES) return -1;

    const int was_mapped = s_mmap;   /* 进来时是映射模式 → 出去前必须恢复，否则调用方的映射指针全废 */
    exit_mmap();
    cmd_init(&cmd);
    cmd.Instruction = 0x03u;                    /* Read Data（单线，够校验用） */
    cmd.Address     = offset;
    cmd.AddressMode = QSPI_ADDRESS_1_LINE;
    cmd.DataMode    = QSPI_DATA_1_LINE;
    cmd.NbData      = len;
    if (HAL_QSPI_Command(&hqspi, &cmd, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        if (was_mapped) (void)mmap_config();
        return -2;
    }
    if (HAL_QSPI_Receive(&hqspi, (uint8_t *)buf, HAL_QSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK) {
        if (was_mapped) (void)mmap_config();
        return -3;
    }
    if (was_mapped) (void)mmap_config();        /* 恢复内存映射 */
    return 0;
}

void synth_port_qspi_cache_invalidate(void) {
    /* 0x90000000 现在是可缓存区（MPU Region2：Normal/可缓存/只读/XN）⇒ 写完必须让缓存失效，
       否则 CPU 读到的还是写之前的旧行。整片 clean+invalidate 足够（导入不在音频路径上）。 */
    SCB_CleanInvalidateDCache();
}
