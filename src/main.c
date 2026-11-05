#include "py32f0xx_hal.h"

/* 点灯测试入口，对应默认的 scale_py32f002a 编译环境。
 * SDK 启动代码使用内部 8MHz HSI。PB0 连接 LED2，PA13/PA14 保持用于 SWD。 */
int main(void)
{
    HAL_Init(); /* 初始化 HAL 和 SysTick，提供 HAL_Delay 的毫秒计时。 */

    /* 打开 GPIOB 时钟，先写初始电平，再配置 PB0 为低速推挽输出。 */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    GPIO_InitTypeDef led = {0};
    led.Pin = GPIO_PIN_0;
    led.Mode = GPIO_MODE_OUTPUT_PP;
    led.Pull = GPIO_NOPULL;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &led);

    /* 每 500ms 翻转一次，完整亮灭周期约 1 秒，用来确认固件确实在运行。 */
    while (1) {
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
        HAL_Delay(500);
    }
}
