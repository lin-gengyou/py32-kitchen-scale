/* PB1直接检测BAT-SW，无分压。以内部1.2V参考测得实际VDDA，再换算电池电压。
 * 外部供电模式只显示电压，不编造百分比、不触发电池低电告警。
 */
#include "battery.h"
#include "py32f0xx_hal.h"
#include <string.h>

static BatteryState bat_state = {.percent = -1};
static unsigned bat_profile, bat_reference_mv = 1200;
static uint32_t bat_sum, bat_supply_sum, bat_last_poll, bat_conversion_ms;
static uint8_t bat_samples, bat_low_runs, bat_critical_runs, bat_phase;
static unsigned bat_reference_raw;
static bool bat_sleep;

#ifndef SCALE_HOST_TEST
/* ADEN在单次转换结束后回到OFF；每个通道都重新使能并留出模拟建立时间。 */
static void battery_adc_settle(void)
{
    for (volatile unsigned wait=0; wait<32U; ++wait) { __NOP(); }
}
static void battery_adc_start(unsigned channel)
{
    /* 与HAL的ConfigChannel→Start顺序一致：先选择MUX，再置ADEN。
     * 实机若先ADEN再改CHSELR，会采到上一通道，VREF读数可变为满码。
     */
    ADC1->CHSELR = 1UL << channel;
    ADC1->CR |= ADC_CR_ADEN;
    battery_adc_settle();
    ADC1->ISR = ADC_ISR_EOC | ADC_ISR_EOSEQ | ADC_ISR_OVR;
    ADC1->CR |= ADC_CR_ADSTART;
}
#endif

void battery_configure(unsigned selected, unsigned calibration_mv)
{
    bat_profile = selected;
    bat_reference_mv = calibration_mv;
    memset(&bat_state, 0, sizeof(bat_state)); bat_state.percent = -1;
    bat_samples = bat_low_runs = bat_critical_runs = 0; bat_sum = bat_supply_sum = 0;
}

const BatteryState *battery_state(void) { return &bat_state; }

/* 使用电压估算电量；锂电采用非线性查表，干电池/镍氢分别使用自己的范围。 */
static int estimate_percent(unsigned mv)
{
    static const uint16_t lithium[] = {3000,3300,3500,3600,3700,3800,3900,4000,4100,4200};
    static const uint8_t percent[] = {0,5,10,20,40,60,75,88,95,100};
    if (bat_profile == BATTERY_EXTERNAL) { return -1; }
    if (bat_profile == BATTERY_LIION_1S) {
        if (mv <= lithium[0]) { return 0; }
        for (unsigned i = 1; i < sizeof(percent); ++i) {
            if (mv < lithium[i]) {
                return percent[i-1] + (int)((mv-lithium[i-1]) * (percent[i]-percent[i-1]) /
                    (lithium[i]-lithium[i-1]));
            }
        }
        return 100;
    }
    unsigned empty = 2000, full = bat_profile == BATTERY_ALKALINE_2S ? 3200U : 2800U;
    return mv <= empty ? 0 : mv >= full ? 100 : (int)((mv-empty)*100U/(full-empty));
}

void battery_accept_raw(unsigned raw, unsigned reference, uint32_t now)
{
    (void)now;
    if (raw > 4095U || reference < 800U || reference > 3000U) {
        bat_state.valid = false; bat_state.percent = -1; bat_state.status = BATTERY_ADC_ERROR;
        bat_samples = 0; bat_sum = bat_supply_sum = 0; return;
    }
    unsigned supply = (4095U * bat_reference_mv + reference/2U) / reference;
    if (supply < 1800U || supply > 5500U) {
        bat_state.valid = false; bat_state.percent = -1; bat_state.status = BATTERY_ADC_ERROR;
        bat_samples = 0; bat_sum = bat_supply_sum = 0; return;
    }
    bat_sum += (raw * bat_reference_mv + reference/2U) / reference;
    bat_supply_sum += supply;
    if (++bat_samples < 8U) { return; }
    bat_state.millivolts = (uint16_t)((bat_sum + 4U) / 8U);
    bat_state.supply_mv = (uint16_t)((bat_supply_sum + 4U) / 8U);
    bat_sum = bat_supply_sum = bat_samples = 0;
    bat_state.valid = true; bat_state.percent = (int8_t)estimate_percent(bat_state.millivolts);
    if (bat_profile == BATTERY_EXTERNAL) { bat_state.status = BATTERY_OK; return; }
    unsigned low = bat_profile == BATTERY_LIION_1S ? 3300U : 2100U;
    unsigned critical = bat_profile == BATTERY_LIION_1S ? 3000U : bat_profile == BATTERY_ALKALINE_2S ? 1800U : 1900U;
    bat_low_runs = bat_state.millivolts < low ? (bat_low_runs < 3U ? bat_low_runs + 1U : 3U) : 0;
    bat_critical_runs = bat_state.millivolts < critical ? (bat_critical_runs < 3U ? bat_critical_runs + 1U : 3U) : 0;
    if (bat_critical_runs >= 3U) { bat_state.status = BATTERY_CRITICAL; }
    else if (bat_low_runs >= 3U && (bat_state.status != BATTERY_CRITICAL || bat_state.millivolts > critical + 100U)) { bat_state.status = BATTERY_LOW; }
    else if (bat_state.status == BATTERY_WAIT || bat_state.status == BATTERY_ADC_ERROR || bat_state.millivolts > low + 100U) {
        bat_state.status = BATTERY_OK;
    }
}

void battery_init(uint32_t now)
{
    bat_last_poll = now; bat_phase = 0; bat_samples = bat_low_runs = bat_critical_runs = 0;
    bat_sum = bat_supply_sum = 0;
    bat_state.valid = false; bat_state.percent = -1; bat_state.status = BATTERY_WAIT;
#ifndef SCALE_HOST_TEST
    __HAL_RCC_GPIOB_CLK_ENABLE(); __HAL_RCC_ADC_CLK_ENABLE();
    __HAL_RCC_ADC_FORCE_RESET(); __HAL_RCC_ADC_RELEASE_RESET();
    GPIO_InitTypeDef pin = {.Pin = GPIO_PIN_1, .Mode = GPIO_MODE_ANALOG, .Pull = GPIO_NOPULL};
    HAL_GPIO_Init(GPIOB, &pin);
    ADC1->CR = 0; ADC1->CFGR1 = 0; ADC1->CFGR2 = ADC_CFGR2_CKMODE_1;
    ADC1->CR |= ADC_CR_ADCAL;
    uint32_t start = HAL_GetTick();
    while ((ADC1->CR & ADC_CR_ADCAL) != 0U) {
        if ((uint32_t)(HAL_GetTick() - start) > 2U) {
            bat_state.status = BATTERY_ADC_ERROR; return;
        }
    }
    /* ADCAL清零后的4个ADC时钟内不能置ADEN。 */
    battery_adc_settle();
    ADC1->CFGR2 = ADC_CFGR2_CKMODE_1; /* PCLK/4，2MHz转换时钟。 */
    ADC1->SMPR = ADC_SMPR_SMP; ADC->CCR |= ADC_CCR_VREFEN;
    /* 每次转换由battery_adc_start按通道重新使能。 */
#endif
}

/* 每125ms启动一轮，转换通过EOC轮询推进，不在采样主循环里等待ADC。 */
void battery_poll(uint32_t now, bool sleeping)
{
    if (sleeping) {
        if (!bat_sleep) {
            bat_sleep = true; bat_phase = bat_samples = 0; bat_sum = bat_supply_sum = 0;
            bat_state.valid = false; bat_state.percent = -1; bat_state.status = BATTERY_WAIT;
#ifndef SCALE_HOST_TEST
            __HAL_RCC_ADC_CLK_DISABLE();
#endif
        }
        return;
    }
    if (bat_sleep) { bat_sleep = false; battery_init(now); return; }
#ifndef SCALE_HOST_TEST
    /* 瞬态转换故障每5秒重新初始化，恢复后重新累积8组新数据。 */
    if (bat_state.status == BATTERY_ADC_ERROR) {
        if ((uint32_t)(now - bat_last_poll) >= 5000U) { battery_init(now); }
        return;
    }
    if (bat_phase == 0U) {
        if ((uint32_t)(now - bat_last_poll) < 125U) { return; }
        bat_last_poll = bat_conversion_ms = now;
        battery_adc_start(12); bat_phase = 1;
    } else if ((ADC1->ISR & ADC_ISR_EOC) != 0U) {
        unsigned raw = ADC1->DR;
        if (bat_phase == 1U) {
            bat_reference_raw = raw;
            battery_adc_start(9); bat_phase = 2;
        } else { battery_accept_raw(raw, bat_reference_raw, now); bat_phase = 0; }
    } else if ((uint32_t)(now - bat_conversion_ms) > 5U) {
        bat_state.valid = false; bat_state.percent = -1; bat_state.status = BATTERY_ADC_ERROR; bat_phase = 0;
    }
#else
    (void)now; (void)sleeping; (void)bat_conversion_ms; (void)bat_reference_raw;
#endif
}
