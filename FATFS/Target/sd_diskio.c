/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sd_diskio.c
  * @brief   SD Disk I/O driver
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

/* Note: code generation based on sd_diskio_template_bspv1.c v2.1.4
   as "Use dma template" is disabled. */

/* USER CODE BEGIN firstSection */
/* can be used to modify / undefine following code or add new definitions */
/* USER CODE END firstSection*/

/* Includes ------------------------------------------------------------------*/
#include "ff_gen_drv.h"
#include "sd_diskio.h"
#include "sdmmc.h"      /* hsd1：超时中止（HAL_SD_Abort）要用到句柄 */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* use the default SD timout as defined in the platform BSP driver*/
#if defined(SDMMC_DATATIMEOUT)
#define SD_TIMEOUT SDMMC_DATATIMEOUT
#elif defined(SD_DATATIMEOUT)
#define SD_TIMEOUT SD_DATATIMEOUT
#else
#define SD_TIMEOUT 30 * 1000
#endif

#define SD_DEFAULT_BLOCK_SIZE 512

/*
 * Depending on the use case, the SD card initialization could be done at the
 * application level: if it is the case define the flag below to disable
 * the BSP_SD_Init() call in the SD_Initialize() and add a call to
 * BSP_SD_Init() elsewhere in the application.
 */
/* USER CODE BEGIN disableSDInit */
/* #define DISABLE_SD_INIT */
/* USER CODE END disableSDInit */

/* Private variables ---------------------------------------------------------*/
/* Disk status */
static volatile DSTATUS Stat = STA_NOINIT;

/* Private function prototypes -----------------------------------------------*/
static DSTATUS SD_CheckStatus(BYTE lun);
DSTATUS SD_initialize (BYTE);
DSTATUS SD_status (BYTE);
DRESULT SD_read (BYTE, BYTE*, DWORD, UINT);
#if _USE_WRITE == 1
DRESULT SD_write (BYTE, const BYTE*, DWORD, UINT);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
DRESULT SD_ioctl (BYTE, BYTE, void*);
#endif  /* _USE_IOCTL == 1 */

const Diskio_drvTypeDef  SD_Driver =
{
  SD_initialize,
  SD_status,
  SD_read,
#if  _USE_WRITE == 1
  SD_write,
#endif /* _USE_WRITE == 1 */

#if  _USE_IOCTL == 1
  SD_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* USER CODE BEGIN beforeFunctionSection */
/* can be used to modify / undefine following code or add new code */
/* USER CODE END beforeFunctionSection */

/* Private functions ---------------------------------------------------------*/

static DSTATUS SD_CheckStatus(BYTE lun)
{
  Stat = STA_NOINIT;

  if(BSP_SD_GetCardState() == MSD_OK)
  {
    Stat &= ~STA_NOINIT;
  }

  return Stat;
}

/**
  * @brief  Initializes a Drive
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_initialize(BYTE lun)
{
Stat = STA_NOINIT;

#if !defined(DISABLE_SD_INIT)

  if(BSP_SD_Init() == MSD_OK)
  {
    Stat = SD_CheckStatus(lun);
  }

#else
  Stat = SD_CheckStatus(lun);
#endif

  return Stat;
}

/**
  * @brief  Gets Disk Status
  * @param  lun : not used
  * @retval DSTATUS: Operation status
  */
DSTATUS SD_status(BYTE lun)
{
  return SD_CheckStatus(lun);
}

/* USER CODE BEGIN beforeReadSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeReadSection */
/**
  * @brief  Reads Sector(s)
  * @param  lun : not used
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */

#define SD_DMA_TIMEOUT_MS 500u
static volatile uint8_t g_sd_busy = 0u;   /* 1 = 有一笔 DMA 传输在跑（回调里清零） */

/* 本地补丁（CubeMX 重新生成会覆盖，见 docs/handoff.md）：
   改用 SDMMC 内部 IDMA 的 DMA 传输 + 完成回调等待，**不再用轮询模式**。
   原因：BSP_SD_ReadBlocks()（= HAL_SD_ReadBlocks）靠 CPU 逐字读 RX FIFO，而音频 DMA 中断
   优先级是 0 —— 只要被它抢占超过 FIFO 深度就会 RXOVERR，更糟的是软件超时是 30 s（SD_TIMEOUT），
   主循环会被卡住整分钟：预渲染环喂不上（实测 audio_underruns 每秒涨几百）、USB 也不响应。
   IDMA 自己搬数据，CPU 只等一个标志，就不怕被音频中断打断。 */
void BSP_SD_ReadCpltCallback(void)  { g_sd_busy = 0u; }
void BSP_SD_WriteCpltCallback(void) { g_sd_busy = 0u; }
void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd) { (void)hsd; g_sd_busy = 0u; }

/* 等 DMA 完成；返回 0 = 完成、-1 = 超时（并中止这次传输） */
static int SdWaitDma(void) {
  const uint32_t t0 = HAL_GetTick();
  while (g_sd_busy) {
    if ((uint32_t)(HAL_GetTick() - t0) >= SD_DMA_TIMEOUT_MS) {
      (void)HAL_SD_Abort(&hsd1);
      g_sd_busy = 0u;
      return -1;
    }
  }
  return 0;
}

DRESULT SD_read(BYTE lun, BYTE *buff, DWORD sector, UINT count)
{
  DRESULT res = RES_ERROR;
  (void)lun;

  /* 本地补丁：SDMMC 用芯片内部 IDMA 直接写 AXI SRAM，绕开 D-Cache ——
     传输前清掉脏行（否则回写会盖掉 IDMA 的结果），传输后再 clean+invalidate 让 CPU 看到新数据。 */
  SCB_CleanInvalidateDCache();
  g_sd_busy = 1u;
  if (BSP_SD_ReadBlocks_DMA((uint32_t*)buff, (uint32_t)sector, count) == MSD_OK) {
    if (SdWaitDma() == 0) res = RES_OK;
  }
  g_sd_busy = 0u;
  SCB_CleanInvalidateDCache();

  return res;
}

/* USER CODE BEGIN beforeWriteSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeWriteSection */
/**
  * @brief  Writes Sector(s)
  * @param  lun : not used
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1

DRESULT SD_write(BYTE lun, const BYTE *buff, DWORD sector, UINT count)
{
  DRESULT res = RES_ERROR;

  /* 本地补丁：写之前必须把 CPU 写进 D-Cache 的数据清到 SRAM（IDMA 从 SRAM 取数）。 */
  SCB_CleanInvalidateDCache();
  if(BSP_SD_WriteBlocks((uint32_t*)buff,
                        (uint32_t)(sector),
                        count, SD_TIMEOUT) == MSD_OK)
  {
	/* wait until the Write operation is finished */
    while(BSP_SD_GetCardState() != MSD_OK)
    {
    }
    res = RES_OK;
  }

  return res;
}
#endif /* _USE_WRITE == 1 */

/* USER CODE BEGIN beforeIoctlSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END beforeIoctlSection */
/**
  * @brief  I/O control operation
  * @param  lun : not used
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT SD_ioctl(BYTE lun, BYTE cmd, void *buff)
{
  DRESULT res = RES_ERROR;
  BSP_SD_CardInfo CardInfo;

  if (Stat & STA_NOINIT) return RES_NOTRDY;

  switch (cmd)
  {
  /* Make sure that no pending write process */
  case CTRL_SYNC :
    res = RES_OK;
    break;

  /* Get number of sectors on the disk (DWORD) */
  case GET_SECTOR_COUNT :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockNbr;
    res = RES_OK;
    break;

  /* Get R/W sector size (WORD) */
  case GET_SECTOR_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(WORD*)buff = CardInfo.LogBlockSize;
    res = RES_OK;
    break;

  /* Get erase block size in unit of sector (DWORD) */
  case GET_BLOCK_SIZE :
    BSP_SD_GetCardInfo(&CardInfo);
    *(DWORD*)buff = CardInfo.LogBlockSize / SD_DEFAULT_BLOCK_SIZE;
    res = RES_OK;
    break;

  default:
    res = RES_PARERR;
  }

  return res;
}
#endif /* _USE_IOCTL == 1 */

/* USER CODE BEGIN afterIoctlSection */
/* can be used to modify previous code / undefine following code / add new code */
/* USER CODE END afterIoctlSection */

/* USER CODE BEGIN lastSection */
/* can be used to modify / undefine previous code or add new code */
/* USER CODE END lastSection */
