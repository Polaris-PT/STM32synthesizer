/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : fatfs_platform.c
  * @brief          : fatfs_platform source file
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
#include "fatfs_platform.h"

uint8_t	BSP_PlatformIsDetected(void) {
    /* 本地修改（CubeMX 重新生成会覆盖）：本板没有卡检测脚 —— 实测 PD4 插卡/拔卡恒为高，
       WeAct 官方 04-SD_Test 例程也没有任何检测脚。而 CubeMX 生成的这段判断是
       "PD4 不为低 = 没插卡"，会把插好的卡判成不存在 → FatFs 的 disk_status() 返回
       STA_NOINIT → f_mount() 报 FR_NOT_READY（我们实测到的就是这个 3）。
       卡在不在由后面的 HAL_SD_Init() 说话，所以这里直接报告 SD_PRESENT。 */
    uint8_t status = SD_PRESENT;
    /* USER CODE BEGIN 1 */
    /* user code can be inserted here */
    /* USER CODE END 1 */
    return status;
}
