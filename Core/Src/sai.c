/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : SAI.c
  * Description        : This file provides code for the configuration
  *                      of the SAI instances.
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
#include "sai.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

SAI_HandleTypeDef hsai_BlockB3;
DMA_HandleTypeDef hdma_sai3_b;

/* SAI3 init function */
void MX_SAI3_Init(void)
{

  /* USER CODE BEGIN SAI3_Init 0 */

  /* USER CODE END SAI3_Init 0 */

  /* USER CODE BEGIN SAI3_Init 1 */

  /* USER CODE END SAI3_Init 1 */

  hsai_BlockB3.Instance = SAI3_Block_B;
  hsai_BlockB3.Init.AudioMode = SAI_MODEMASTER_TX;
  hsai_BlockB3.Init.Synchro = SAI_ASYNCHRONOUS;
  hsai_BlockB3.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockB3.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE;
  hsai_BlockB3.Init.MckOverSampling = SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockB3.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockB3.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_48K;
  hsai_BlockB3.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
  hsai_BlockB3.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockB3.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockB3.Init.TriState = SAI_OUTPUT_NOTRELEASED;
  if (HAL_SAI_InitProtocol(&hsai_BlockB3, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_32BIT, 2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SAI3_Init 2 */

  /* USER CODE END SAI3_Init 2 */

}
static uint32_t SAI3_client =0;

void HAL_SAI_MspInit(SAI_HandleTypeDef* saiHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
/* SAI3 */
    if(saiHandle->Instance==SAI3_Block_B)
    {
      /* SAI3 clock enable */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SAI3;
    PeriphClkInitStruct.PLL2.PLL2M = 5;
    PeriphClkInitStruct.PLL2.PLL2N = 108;
    PeriphClkInitStruct.PLL2.PLL2P = 22;
    PeriphClkInitStruct.PLL2.PLL2Q = 2;
    PeriphClkInitStruct.PLL2.PLL2R = 2;
    PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_2;
    PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
    PeriphClkInitStruct.PLL2.PLL2FRACN = 1101;
    PeriphClkInitStruct.Sai23ClockSelection = RCC_SAI23CLKSOURCE_PLL2;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

      if (SAI3_client == 0)
      {
       __HAL_RCC_SAI3_CLK_ENABLE();
      }
    SAI3_client ++;

    /**SAI3_B_Block_B GPIO Configuration
    PD8     ------> SAI3_SCK_B
    PD9     ------> SAI3_SD_B
    PD10     ------> SAI3_FS_B
    PD14     ------> SAI3_MCLK_B
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_14;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF6_SAI3;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /* Peripheral DMA init*/

    hdma_sai3_b.Instance = DMA1_Stream0;
    hdma_sai3_b.Init.Request = DMA_REQUEST_SAI3_B;
    hdma_sai3_b.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_sai3_b.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_sai3_b.Init.MemInc = DMA_MINC_ENABLE;
    hdma_sai3_b.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_sai3_b.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    hdma_sai3_b.Init.Mode = DMA_CIRCULAR;
    hdma_sai3_b.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    hdma_sai3_b.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_sai3_b) != HAL_OK)
    {
      Error_Handler();
    }

    /* Several peripheral DMA handle pointers point to the same DMA handle.
     Be aware that there is only one channel to perform all the requested DMAs. */
    __HAL_LINKDMA(saiHandle,hdmarx,hdma_sai3_b);
    __HAL_LINKDMA(saiHandle,hdmatx,hdma_sai3_b);
    }
}

void HAL_SAI_MspDeInit(SAI_HandleTypeDef* saiHandle)
{

/* SAI3 */
    if(saiHandle->Instance==SAI3_Block_B)
    {
    SAI3_client --;
      if (SAI3_client == 0)
      {
      /* Peripheral clock disable */
      __HAL_RCC_SAI3_CLK_DISABLE();
      }

    /**SAI3_B_Block_B GPIO Configuration
    PD8     ------> SAI3_SCK_B
    PD9     ------> SAI3_SD_B
    PD10     ------> SAI3_FS_B
    PD14     ------> SAI3_MCLK_B
    */
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_14);

    HAL_DMA_DeInit(saiHandle->hdmarx);
    HAL_DMA_DeInit(saiHandle->hdmatx);
    }
}

/**
  * @}
  */

/**
  * @}
  */
