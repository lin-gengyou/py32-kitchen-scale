#ifndef SCALE_PY32_HAL_CONF_H
#define SCALE_PY32_HAL_CONF_H

/* 启用本工程需要的 HAL 模块。串口在基础版中直接操作寄存器。 */
#define HAL_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED

/* 内部 HSI 为实际系统时钟，HSE/LSE 数值仅供 HAL 计算和配置接口使用。 */
#define HSI_VALUE 8000000U
#define HSE_VALUE 24000000U
#define HSE_STARTUP_TIMEOUT 200U
#define LSI_VALUE 32768U
#define LSE_VALUE 32768U
#define LSE_STARTUP_TIMEOUT 5000U
/* 本板 MCU 的设计供电为 5V，毫秒节拍中断使用最低优先级。 */
#define VDD_VALUE 5000U
#define TICK_INT_PRIORITY 3U
#define USE_RTOS 0U
#define PREFETCH_ENABLE 0U
#define assert_param(expr) ((void)0U)

/* HAL 总头文件通过此配置包含对应驱动声明。 */
#include "py32f0xx_hal_rcc.h"
#include "py32f0xx_hal_flash.h"
#include "py32f0xx_hal_gpio.h"
#include "py32f0xx_hal_cortex.h"
#include "py32f0xx_hal_pwr.h"
#include "py32f0xx_hal_iwdg.h"

#endif
