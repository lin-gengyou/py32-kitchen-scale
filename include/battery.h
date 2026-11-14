#ifndef SCALE_BATTERY_H
#define SCALE_BATTERY_H
#include <stdbool.h>
#include <stdint.h>

enum { BATTERY_EXTERNAL, BATTERY_LIION_1S, BATTERY_ALKALINE_2S, BATTERY_NIMH_2S };
enum { BATTERY_WAIT, BATTERY_OK, BATTERY_LOW, BATTERY_CRITICAL, BATTERY_ADC_ERROR };
typedef struct {
    uint16_t millivolts, supply_mv;
    int8_t percent;              /* -1表示外部供电或尚未有有效采样。 */
    uint8_t status;
    bool valid;
} BatteryState;

void battery_configure(unsigned profile, unsigned reference_mv);
void battery_init(uint32_t now);
void battery_poll(uint32_t now, bool sleeping);
const BatteryState *battery_state(void);
/* 算法入口也用于主机检查：PB1与内部VREFINT的同一轮ADC原始值。 */
void battery_accept_raw(unsigned raw, unsigned reference, uint32_t now);
#endif
