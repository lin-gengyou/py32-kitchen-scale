#include "py32f0xx_hal.h"

__attribute__((weak)) void scale_tick_1ms(void) {}
__attribute__((weak)) void scale_uart_irq(void) {}

void USART1_IRQHandler(void) { scale_uart_irq(); }

/* SysTick 每 1ms 进入一次，维护 HAL_GetTick 使用的计数器。 */
void SysTick_Handler(void)
{
    HAL_IncTick();
    scale_tick_1ms();
}

/* 严重运行异常时停在这里，可连接调试器查看寄存器和调用现场。 */
void HardFault_Handler(void)
{
    while (1) {
        /* 保留异常现场，等待调试器。 */
    }
}
