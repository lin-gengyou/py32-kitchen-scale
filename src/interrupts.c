#include "py32f0xx_hal.h"

/* SysTick 每 1ms 进入一次，维护 HAL_GetTick 使用的计数器。 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* 严重运行异常时停在这里，可连接调试器查看寄存器和调用现场。 */
void HardFault_Handler(void)
{
    while (1) {
        /* 保留异常现场，等待调试器。 */
    }
}
