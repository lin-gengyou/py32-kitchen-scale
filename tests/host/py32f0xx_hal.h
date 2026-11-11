#ifndef SCALE_HOST_HAL_H
#define SCALE_HOST_HAL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* 供桌面回归检查使用，只模拟本固件涉及的 GPIO/USART 接口。 */
typedef struct { volatile uint32_t IDR, BSRR, ODR; } GPIO_TypeDef;
typedef struct { volatile uint32_t SR, DR, CR1, CR2, CR3, BRR; } USART_TypeDef;
typedef struct { uint32_t Mode, Pull, Speed, Pin, Alternate; } GPIO_InitTypeDef;
extern GPIO_TypeDef host_gpio_a, host_gpio_b, host_gpio_f;
extern USART_TypeDef host_usart;
extern bool host_dbg_clock_enabled, host_iwdg_frozen;
#define GPIOA (&host_gpio_a)
#define GPIOB (&host_gpio_b)
#define GPIOF (&host_gpio_f)
#define USART1 (&host_usart)
#define GPIO_PIN_0 1U
#define GPIO_PIN_1 2U
#define GPIO_PIN_2 4U
#define GPIO_PIN_3 8U
#define GPIO_PIN_4 16U
#define GPIO_PIN_5 32U
#define GPIO_PIN_6 64U
#define GPIO_PIN_7 128U
#define GPIO_PIN_SET 1U
#define GPIO_PIN_RESET 0U
#define GPIO_MODE_OUTPUT_PP 1U
#define GPIO_MODE_INPUT 0U
#define GPIO_MODE_AF_PP 2U
#define GPIO_NOPULL 0U
#define GPIO_PULLUP 1U
#define GPIO_SPEED_FREQ_LOW 0U
#define GPIO_AF1_USART1 1U
#define USART_CR1_TE 8U
#define USART_CR1_RE 4U
#define USART_CR1_UE 8192U
#define USART_SR_ORE 8U
#define USART_SR_NE 4U
#define USART_SR_FE 2U
#define USART_SR_PE 1U
#define USART_SR_RXNE 32U
#define USART_SR_TXE 128U
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOF_CLK_ENABLE() ((void)0)
#define __HAL_RCC_USART1_CLK_ENABLE() ((void)0)
#define __HAL_RCC_DBGMCU_CLK_ENABLE() (host_dbg_clock_enabled = true)
#define __NOP() ((void)0)
#define __disable_irq() ((void)0)
#define __get_PRIMASK() (0U)
#define __set_PRIMASK(mask) ((void)(mask))
#define __WFI() ((void)0)
#define __HAL_DBGMCU_FREEZE_IWDG() (host_iwdg_frozen = host_dbg_clock_enabled)
#define HAL_Init() ((void)0)
#define HAL_GetTick() (0U)
#define HAL_RCC_GetPCLK1Freq() (8000000U)
static inline void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint32_t pin, uint32_t state)
{
    if (state) {
        port->ODR |= pin;
    } else {
        port->ODR &= ~pin;
    }
}
#define HAL_GPIO_Init(port, pin) ((void)(port), (void)(pin))
#define HAL_GPIO_TogglePin(port, pin) ((port)->ODR ^= (pin))

typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { uint32_t TypeErase; uintptr_t PageAddress; uint32_t NbPages; } FLASH_EraseInitTypeDef;
typedef struct { unsigned Lock; } FLASH_ProcessTypeDef;
extern FLASH_ProcessTypeDef pFlash;
#define HAL_UNLOCKED 0U
#define FLASH_TYPEERASE_PAGEERASE 2U
#define FLASH_TYPEPROGRAM_PAGE 1U
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uintptr_t address, uint32_t *data);
typedef struct { void *Instance; struct { uint32_t Prescaler, Reload; } Init; } IWDG_HandleTypeDef;
#define IWDG ((void *)1)
#define IWDG_PRESCALER_256 6U
#define HAL_IWDG_Init(handle) ((void)(handle), HAL_OK)
static inline HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *handle)
{
    (void)handle;
    return HAL_OK;
}
#endif
