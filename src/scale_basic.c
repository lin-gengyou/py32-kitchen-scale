/*
 * 电子秤基础固件，入口对应 PlatformIO 的 scale_basic 环境。
 * 数据流为 HX711 原始计数 → 滤波与稳定判定 → 校准换算 → 去皮 → 段码 LCD 显示。
 * 标定、单位、蜂鸣器、判稳和待机设置通过双页 CRC 记录掉电保存。
 * 时序均由毫秒计时调度，采样、按键、蜂鸣器和屏幕可以交替处理。
 */
#include "py32f0xx_hal.h"
#include "scale_config.h"
#include "weight_state.h"
#include "firmware_version.h"
#include "lcd_ht1621.h"
#include "battery.h"
#include "ble_link.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* 引脚来自本板原理图。PA13/PA14 保留 SWD；PF1 为段码屏 RD。 */
#define HX711_PORT GPIOA
#define HX711_DATA_PIN GPIO_PIN_0
#define HX711_CLOCK_PIN GPIO_PIN_6
#define BUZZER_PORT GPIOA
#define BUZZER_PIN GPIO_PIN_1
#define KEY_PORT GPIOB
#define KEY_PIN GPIO_PIN_3
#define LED_PORT GPIOB
#define LED_PIN GPIO_PIN_0

/* 联调默认值。稳定阈值按 ADC 计数设置，量程和实际精度需要实物确认。 */
enum {
    AVERAGE_SAMPLES = 8,          /* 稳定时平均 8 点，降低显示抖动。 */
    FAST_SAMPLES = 3,             /* 变化时平均最近 3 点，缩短跟随延迟。 */
    HX711_STARTUP_MS = 500,       /* 给上电后的 HX711 留出建立时间。 */
    HX711_TIMEOUT_MS = 1000,      /* 连续 1 秒无有效采样即报告故障。 */
    STABLE_HOLD_MS = 500,         /* 整个窗口持续满足阈值 500ms 才算稳定。 */
    DEFAULT_STABLE_COUNTS = 0,   /* 0自动：按标定换算0.2g窗口；正数为手动计数。 */
    DEFAULT_REPORT_MS = 200,       /* 文档要求 5～10Hz，默认选择 5Hz。 */
    MIN_REPORT_MS = 100,
    MAX_REPORT_MS = 200,
    KEY_DEBOUNCE_MS = 30,
    KEY_LONG_PRESS_MS = 1500,
    BUZZER_ON_MS = 120,
    BUZZER_GAP_MS = 100,
    ALARM_INTERVAL_MS = 2000,
    MAX_GROSS_GRAMS = 5000,
    OL_GRAMS = 5250,
    EMPTY_CONFIRM_MS = 3000,
};

typedef enum {
    BUZZER_OFF,                  /* 器件类型尚未核实，默认静音。 */
    BUZZER_ACTIVE,               /* 有源蜂鸣器，通过高低电平控制通断。 */
    BUZZER_PASSIVE,              /* 无源蜂鸣器，通过约 500Hz 方波发声。 */
} BuzzerMode;

/* 采样环形缓冲区。24 位计数的 8 点累加仍在 int32_t 范围内。 */
static int32_t samples[AVERAGE_SAMPLES];
static int32_t sample_sum;
static uint8_t sample_count;
static uint8_t sample_index;
static uint32_t started_ms;
static uint32_t last_sample_ms;
static bool hx711_started;
static bool sample_fault;
static bool adc_saturated;
static bool settling;
static bool stable;
static uint32_t stable_since_ms;
static uint32_t stable_counts = DEFAULT_STABLE_COUNTS;
static int32_t stable_anchor;

/* 运行时空载与去皮偏移。标定的各段计数允许为负，适配输出方向相反的传感器。 */
static int32_t zero_counts;
static int32_t tare_counts;
static uint32_t reference_grams;
static bool zero_set;

/* 蜂鸣器使用分段计时，响声之间继续运行采样和按键。 */
static volatile BuzzerMode buzzer_mode = BUZZER_OFF;
static volatile uint8_t buzzer_remaining;
static volatile bool buzzer_on;
static uint32_t buzzer_phase_ms;
static bool startup_zero_pending;

/* 文档V1.0的持久配置与统一状态；外部供电的电池百分比使用-1。 */
static ScaleConfig saved_config;
WeightState weight_state = {.battery_percent = -1, .division_mg = 100};
static int32_t raw_samples[3];
static uint8_t raw_count;
static uint8_t raw_index;
static int32_t latest_raw;
static bool empty_reference_known;
static bool empty_candidate;
static uint32_t empty_since_ms;
static uint32_t zero_track_ms;
static uint32_t idle_since_ms;
static bool sleeping;
static IWDG_HandleTypeDef watchdog;
static bool watchdog_ready;

static void weight_update(uint32_t now);
static void wake_scale(uint32_t now);
static void enter_standby(void);


/* 设置输出初值后再切换方向，降低上电误触发蜂鸣器和显示片选的概率。 */
static void board_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(HX711_PORT, HX711_CLOCK_PIN | BUZZER_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET); /* TFT 片选空闲为高。 */
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET); /* 段码 LCD 片选空闲为高。 */

    GPIO_InitTypeDef pin = {
        .Mode = GPIO_MODE_OUTPUT_PP,
        .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_LOW,
        .Pin = HX711_CLOCK_PIN | BUZZER_PIN | GPIO_PIN_4,
    };
    HAL_GPIO_Init(GPIOA, &pin);
    pin.Pin = LED_PIN | GPIO_PIN_2;
    HAL_GPIO_Init(GPIOB, &pin);

    pin.Mode = GPIO_MODE_INPUT;
    pin.Pin = HX711_DATA_PIN;
    HAL_GPIO_Init(HX711_PORT, &pin);
    pin.Pin = KEY_PIN;
    pin.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KEY_PORT, &pin);

    /* PA2/PA3由ble_configure配置为ECB01C透传接口。 */
}

/* 固定 8MHz HSI 下，8 条 NOP 提供约 1us 的边沿间隔，另有指令执行开销。 */
static void hx711_edge_delay(void)
{
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
}

/* DOUT 拉低后读取 24 位有符号数据，第 25 个脉冲选择 A 通道、128 倍增益。 */
static bool hx711_read(int32_t *counts)
{
    uint32_t bits = 0;
    uint32_t irq_mask = __get_PRIMASK();
    /* 只在短暂移位期间关中断，避免时钟高电平被中断拉长而进入掉电模式。 */
    __disable_irq();
    hx711_edge_delay();
    for (unsigned pulse = 0; pulse < 25; ++pulse) {
        HX711_PORT->BSRR = HX711_CLOCK_PIN;
        hx711_edge_delay();
        if (pulse < 24) {
            bits = (bits << 1) | ((HX711_PORT->IDR & HX711_DATA_PIN) != 0U);
        }
        HX711_PORT->BSRR = (uint32_t)HX711_CLOCK_PIN << 16;
        hx711_edge_delay();
    }
    bool released = (HX711_PORT->IDR & HX711_DATA_PIN) != 0U;
    __set_PRIMASK(irq_mask);

    adc_saturated = bits == 0x800000U || bits == 0x7fffffU;
    if (!released || adc_saturated) {
        return false;
    }
    *counts = (bits & 0x800000U) ? (int32_t)bits - 0x1000000 : (int32_t)bits;
    return true;
}

/* 故障或阈值变化后清空旧窗口，防止拿旧数据归零、去皮或判定稳定。 */
static void sample_reset(void)
{
    sample_count = 0;
    sample_index = 0;
    sample_sum = 0;
    settling = false;
    stable = false;
    raw_count = 0;
    raw_index = 0;
}

/* 自动判稳使用0.2g峰峰值；未标定时以128计数建立第一次标定。
 * 手动正数仍严格按菜单输入计数；参数0代表自动，避免不同传感器灵敏度改变门限。
 */
static uint32_t stability_limit(void)
{
    if (stable_counts != 0U) { return stable_counts; }
    if (saved_config.point_count == 0U) { return 128U; }
    int64_t span = saved_config.spans[0];
    if (span < 0) { span = -span; }
    uint32_t divisor = saved_config.grams[0] * 1000U;
    uint32_t limit = (uint32_t)((span * 200 + divisor - 1U) / divisor);
    return limit != 0U ? limit : 1U;
}

/* 进入稳定需完整8点窗口持续500ms；退出使用2倍峰峰值门限。
 * 均值相对进入时的锚点移动一个门限也退出，避免缓慢加料一直挂着S。
 * 数值仍每次采样更新，不锁死重量，不将微小负载吞成零。
 */
static void sample_add(int32_t counts, uint32_t now)
{
    last_sample_ms = now;
    sample_fault = false;
    adc_saturated = false;
    latest_raw = counts;
    raw_samples[raw_index] = counts;
    raw_index = (raw_index + 1U) % 3U;
    if (raw_count < 3U) {
        ++raw_count;
        if (raw_count < 3U) {
            return;
        }
    }
    /* 三点中值抑制单个尖峰，连续加料和持续阶跃仍会进入后续滤波。 */
    int32_t a = raw_samples[0];
    int32_t b = raw_samples[1];
    int32_t c = raw_samples[2];
    if (a > b) { int32_t swap = a; a = b; b = swap; }
    if (b > c) { b = c; }
    counts = a > b ? a : b;
    if (sample_count == AVERAGE_SAMPLES) {
        sample_sum -= samples[sample_index];
    } else {
        ++sample_count;
    }
    samples[sample_index] = counts;
    sample_sum += counts;
    sample_index = (sample_index + 1U) % AVERAGE_SAMPLES;

    int32_t minimum = samples[0];
    int32_t maximum = samples[0];
    for (unsigned i = 1; i < sample_count; ++i) {
        if (samples[i] < minimum) {
            minimum = samples[i];
        }
        if (samples[i] > maximum) {
            maximum = samples[i];
        }
    }
    uint32_t limit = stability_limit();
    uint32_t spread = (uint32_t)(maximum - minimum);
    if (stable) {
        int32_t shift = sample_sum / AVERAGE_SAMPLES - stable_anchor;
        if (shift < 0) { shift = -shift; }
        if (spread <= limit * 2U && (uint32_t)shift <= limit) { return; }
        settling = false; stable = false; return;
    }
    if (sample_count < AVERAGE_SAMPLES || spread > limit) {
        settling = false;
        stable = false;
        return;
    }
    if (!settling) {
        settling = true;
        stable_since_ms = now;
    }
    stable = (uint32_t)(now - stable_since_ms) >= STABLE_HOLD_MS;
    if (stable) { stable_anchor = sample_sum / AVERAGE_SAMPLES; }
}

/* 查询数据就绪脚，有新结果就读取，实际采样速度取决于 HX711 的 RATE 硬件。 */
static void sample_poll(uint32_t now)
{
    if (sleeping) {
        return;
    }
    /* 启动延时只执行一次，毫秒计数回绕后继续正常采样。 */
    if (!hx711_started) {
        if ((uint32_t)(now - started_ms) < HX711_STARTUP_MS) {
            return;
        }
        hx711_started = true;
    }
    if ((uint32_t)(now - last_sample_ms) >= HX711_TIMEOUT_MS) {
        sample_fault = true;
        sample_reset();
    }
    if ((HX711_PORT->IDR & HX711_DATA_PIN) != 0U) {
        return;
    }

    int32_t counts;
    if (!hx711_read(&counts)) {
        sample_fault = true;
        sample_reset();
        return;
    }
    sample_add(counts, now);
}

/* 检查数据的新鲜度，避免刚过采样超时点时还使用上一次的稳定标志。 */
static bool samples_ready(uint32_t now)
{
    return sample_count >= FAST_SAMPLES && !sample_fault &&
           (uint32_t)(now - last_sample_ms) < HX711_TIMEOUT_MS;
}

/* 校准、归零、去皮统一使用完整 8 点平均，保持偏移和换算系数一致。 */
static int32_t average_counts(void)
{
    return sample_sum / sample_count;
}

/* 不稳定时使用最近 3 点平均，稳定后切回 8 点平均。仅在窗口完整时调用。 */
static int32_t filtered_counts(void)
{
    if (stable) {
        return average_counts();
    }
    int32_t total = 0;
    for (unsigned i = 0; i < FAST_SAMPLES; ++i) {
        unsigned index = (sample_index + AVERAGE_SAMPLES - 1U - i) % AVERAGE_SAMPLES;
        total += samples[index];
    }
    return total / FAST_SAMPLES;
}

/* 标定点以空载为原点。选择相邻分段插值，负重量和末段以对应斜率外推。 */
static int64_t gross_milligrams(int32_t counts)
{
    int direction = saved_config.spans[0] < 0 ? -1 : 1;
    int64_t x = ((int64_t)counts - zero_counts) * direction;
    int64_t start_x = 0;
    int64_t start_mg = 0;
    for (unsigned i = 0; i < saved_config.point_count; ++i) {
        int64_t end_x = (int64_t)saved_config.spans[i] * direction;
        int64_t end_mg = (int64_t)saved_config.grams[i] * 1000;
        if (x <= end_x || i + 1U == saved_config.point_count) {
            return start_mg + (x - start_x) * (end_mg - start_mg) / (end_x - start_x);
        }
        start_x = end_x;
        start_mg = end_mg;
    }
    return 0;
}

/* 去皮前已确认稳定，将旧滤波状态清空并用该次实测平均值初始化新窗口。 */
static void filter_seed(int32_t counts, uint32_t now)
{
    sample_reset();
    for (unsigned i = 0; i < FAST_SAMPLES + 2U; ++i) {
        sample_add(counts, now);
    }
}

/* 持久设置只在新页验证成功后生效，写入失败保留之前的 RAM 与 Flash 参数。 */
static bool save_settings(const ScaleConfig *candidate)
{
    if (!scale_config_save(candidate)) {
        return false;
    }
    if (candidate->battery_profile != saved_config.battery_profile ||
        candidate->battery_reference_mv != saved_config.battery_reference_mv) {
        battery_configure(candidate->battery_profile, candidate->battery_reference_mv);
    }
    lcd_set_missing_mask(candidate->lcd_missing_mask);
    if (candidate->bluetooth_enabled != saved_config.bluetooth_enabled) {
        ble_configure(candidate->bluetooth_enabled != 0U);
    }
    saved_config = *candidate;
    stable_counts = candidate->stable_counts;
    buzzer_mode = (BuzzerMode)candidate->buzzer_mode;
    if (buzzer_mode == BUZZER_OFF) {
        buzzer_remaining = 0; buzzer_on = false;
        HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
    }
    return true;
}

/* 按毛重检查上限，容器去皮后依然计入传感器承受的负载。 */
static bool overloaded(uint32_t now)
{
    return reference_grams != 0U && samples_ready(now) &&
           gross_milligrams(filtered_counts()) > (int64_t)MAX_GROSS_GRAMS * 1000;
}

/* 检查测量操作的共同前提，并返回本机界面可显示的原因。NULL 表示允许执行。 */
static const char *measurement_error(uint32_t now, bool check_capacity)
{
    if (saved_config.battery_profile != 0U && battery_state()->status >= BATTERY_LOW) {
        return "ERR,POWER\r\n";
    }
    if (!samples_ready(now) || (saved_config.battery_profile != 0U && !battery_state()->valid)) {
        return "ERR,WAIT_SAMPLES\r\n";
    }
    if (check_capacity && overloaded(now)) {
        return "ERR,OVERLOAD\r\n";
    }
    return stable ? NULL : "ERR,UNSTABLE\r\n";
}

/* 记录当前空秤基线。已存在的计数/克比例继续有效，去皮同时清除。 */
static const char *set_zero(uint32_t now)
{
    const char *error = measurement_error(now, true);
    if (error != NULL) {
        return error;
    }
    /* 已标定时只能在原空秤基线附近归零，防止抹掉 0.5g 等真实小负载。 */
    if (reference_grams != 0U) {
        int64_t gross = gross_milligrams(average_counts());
        if (gross < -100 || gross > 100) {
            return "ERR,NOT_EMPTY\r\n";
        }
    }
    zero_counts = average_counts();
    startup_zero_pending = false;
    zero_set = true;
    empty_reference_known = true;
    empty_candidate = false;
    tare_counts = 0;
    weight_update(now);
    return "OK,ZERO\r\n";
}

/* 皮重仅改变净重偏移。重复去皮会替换上次皮重，毛重基线保持独立。 */
static const char *set_tare(uint32_t now)
{
    if (reference_grams == 0U) {
        return "ERR,NEED_CAL\r\n";
    }
    const char *error = measurement_error(now, true);
    if (error != NULL) {
        return error;
    }
    int32_t counts = average_counts();
    tare_counts = counts - zero_counts;
    empty_candidate = false;
    filter_seed(counts, now);
    weight_update(now);
    return "OK,TARE\r\n";
}

/* 启动一组提示音。成功 1 声，操作失败 3 声，故障/超载报警 3 声。 */
static void buzzer_start(uint8_t pulses, uint32_t now)
{
    if (buzzer_mode == BUZZER_OFF) {
        return;
    }
    buzzer_remaining = pulses;
    buzzer_on = true;
    buzzer_phase_ms = now;
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
}

/* 有源模式维持电平，无源模式每毫秒翻转，产生约 500Hz 的联调提示音。 */
static void buzzer_poll(uint32_t now)
{
    if (buzzer_remaining == 0U) {
        return;
    }
    uint32_t duration = buzzer_on ? BUZZER_ON_MS : BUZZER_GAP_MS;
    if ((uint32_t)(now - buzzer_phase_ms) >= duration) {
        buzzer_phase_ms = now;
        if (buzzer_on) {
            --buzzer_remaining;
            buzzer_on = false;
            HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
        } else {
            buzzer_on = true;
            HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
        }
    }

}

/* 1ms中断只翻转蜂鸣器，不做采样/Flash/界面；彩屏刷新时音调也保持500Hz。 */
void scale_tick_1ms(void)
{
    if (buzzer_mode == BUZZER_PASSIVE && buzzer_on && buzzer_remaining != 0U) {
        HAL_GPIO_TogglePin(BUZZER_PORT, BUZZER_PIN);
    }
}
void scale_uart_irq(void) { ble_uart_irq(); }

/* 分度选择保留小滞回区，避免 200g 和 1000g 附近因噪声反复切换。 */
static uint16_t select_division(int32_t net_mg)
{
    int64_t magnitude = net_mg < 0 ? -(int64_t)net_mg : net_mg;
    uint16_t division = weight_state.division_mg;
    if (magnitude > 1001000 || (division == 1000 && magnitude >= 999000)) {
        return 1000;
    }
    if (magnitude > 200500 || (division >= 500 && magnitude >= 199500)) {
        return 500;
    }
    return 100;
}

/* 更新唯一重量状态。量化放在最后，负净重保留；没有低于 2g 强制归零逻辑。 */
static void weight_update(uint32_t now)
{
    weight_state.unit = saved_config.unit;
    const BatteryState *power = battery_state();
    weight_state.battery_percent = power->percent;
    weight_state.stable = false;
    if (sleeping) {
        weight_state.status = WEIGHT_SLEEP;
        weight_state.motion = MOTION_SLEEP;
        return;
    }
    if (saved_config.battery_profile != 0U && power->status >= BATTERY_LOW) {
        weight_state.status = power->status == BATTERY_ADC_ERROR ? WEIGHT_POWER_ERROR : WEIGHT_LOW_BATTERY;
        weight_state.motion = MOTION_FAULT; return;
    }
    if (saved_config.battery_profile != 0U && !power->valid) {
        weight_state.status = WEIGHT_WAIT; weight_state.motion = MOTION_CANDIDATE; return;
    }
    if (sample_fault || adc_saturated || (uint32_t)(now - last_sample_ms) >= HX711_TIMEOUT_MS) {
        weight_state.status = WEIGHT_SENSOR_ERROR;
        weight_state.motion = MOTION_FAULT;
        return;
    }
    if (!samples_ready(now)) {
        weight_state.status = WEIGHT_WAIT;
        weight_state.motion = MOTION_CANDIDATE;
        return;
    }
    if (reference_grams == 0U) {
        weight_state.status = WEIGHT_UNCALIBRATED;
        weight_state.motion = MOTION_CANDIDATE;
        return;
    }
    int64_t gross = gross_milligrams(filtered_counts());
    int64_t tare = gross_milligrams(zero_counts + tare_counts);
    int64_t net = gross - tare;
    if (gross >= (int64_t)OL_GRAMS * 1000) {
        weight_state.status = WEIGHT_OVERLOAD;
        weight_state.motion = MOTION_FAULT;
        return;
    }
    if (gross > (int64_t)MAX_GROSS_GRAMS * 1000 || gross < -(int64_t)MAX_GROSS_GRAMS * 1000 ||
        net < INT32_MIN || net > INT32_MAX || tare < INT32_MIN || tare > INT32_MAX) {
        weight_state.status = WEIGHT_RANGE;
        weight_state.motion = MOTION_FAULT;
        return;
    }
    weight_state.motion = net < weight_state.net_mg ? MOTION_UNLOADING : MOTION_LOADING;
    weight_state.gross_mg = (int32_t)gross;
    weight_state.tare_mg = (int32_t)tare;
    weight_state.net_mg = (int32_t)net;
    weight_state.division_mg = select_division((int32_t)net);
    int64_t magnitude = net < 0 ? -net : net;
    int64_t rounded = ((magnitude + weight_state.division_mg / 2) / weight_state.division_mg) * weight_state.division_mg;
    weight_state.display_mg = net < 0 ? -(int32_t)rounded : (int32_t)rounded;
    weight_state.stable = stable;
    weight_state.status = WEIGHT_OK;
    if (stable) {
        weight_state.motion = MOTION_STABLE;
    } else if (settling) {
        weight_state.motion = MOTION_CANDIDATE;
    }
}

/* 软件待机关闭段码屏、HX711 和 LED；按键轮询和毫秒时基负责唤醒。 */
static void enter_standby(void)
{
    sleeping = true;
    lcd_power(false);
    sample_reset();
    HAL_GPIO_WritePin(HX711_PORT, HX711_CLOCK_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
    buzzer_remaining = 0;
    weight_state.status = WEIGHT_SLEEP;
    weight_state.motion = MOTION_SLEEP;
    weight_state.stable = false;
}

/* 唤醒重新等待 HX711 建立和积累样本，旧重量不会当作新鲜测量发布。 */
static void wake_scale(uint32_t now)
{
    if (!sleeping) {
        return;
    }
    sleeping = false;
    lcd_power(true);
    HAL_GPIO_WritePin(HX711_PORT, HX711_CLOCK_PIN, GPIO_PIN_RESET);
    sample_reset();
    sample_fault = false;
    adc_saturated = false;
    hx711_started = false;
    started_ms = now;
    last_sample_ms = now;
    idle_since_ms = now;
    empty_candidate = false;
    weight_update(now);
}

/* 仅在已知空秤基线附近、无皮重、原始值和滤波值都在 ±0.1g 内时允许跟踪。 */
static void power_poll(uint32_t now)
{
    if (sleeping) {
        return;
    }
    weight_update(now);
    bool empty = empty_reference_known && tare_counts == 0 && weight_state.status == WEIGHT_OK &&
                 weight_state.stable && weight_state.net_mg >= -100 && weight_state.net_mg <= 100;
    if (empty) {
        int64_t raw_mg = gross_milligrams(latest_raw);
        empty = raw_mg >= -100 && raw_mg <= 100;
    }
    if (!empty) {
        empty_candidate = false;
        idle_since_ms = now;
        return;
    }
    if (startup_zero_pending) {
        zero_counts = average_counts(); startup_zero_pending = false; weight_update(now);
    }
    if (!empty_candidate) {
        empty_candidate = true;
        empty_since_ms = now;
        zero_track_ms = now;
    }
    if ((uint32_t)(now - empty_since_ms) < EMPTY_CONFIRM_MS) {
        return;
    }
    weight_state.motion = MOTION_EMPTY;
    if ((uint32_t)(now - zero_track_ms) >= 1000U) {
        /* 每秒只修正偏差的 1/64，跟踪量留在 RAM，避免频繁擦写 Flash。 */
        zero_counts += (average_counts() - zero_counts) / 64;
        zero_track_ms = now;
    }
    if (saved_config.auto_off_seconds != 0U &&
        (uint32_t)(now - idle_since_ms) >= saved_config.auto_off_seconds * 1000U) {
        enter_standby();
    }
}

/* 故障和超量程三声报警，间隔 2 秒重复。静音配置下继续发布真实错误状态。 */
static void alarm_poll(uint32_t now)
{
    static bool alarming;
    static uint32_t alarm_ms;
    bool fault = !sleeping && (weight_state.status == WEIGHT_SENSOR_ERROR ||
                 weight_state.status == WEIGHT_OVERLOAD || weight_state.status == WEIGHT_RANGE ||
                 weight_state.status == WEIGHT_LOW_BATTERY || weight_state.status == WEIGHT_POWER_ERROR);
    if (fault && (!alarming || (uint32_t)(now - alarm_ms) >= ALARM_INTERVAL_MS)) {
        buzzer_start(3, now);
        alarm_ms = now;
    }
    alarming = fault;
}

/* 调试冻结寄存器依赖 DBGMCU 时钟。冻结仅在内核暂停时生效，正常运行仍由看门狗保护。 */
static void watchdog_start(void)
{
    __HAL_RCC_DBGMCU_CLK_ENABLE();
    __HAL_DBGMCU_FREEZE_IWDG();
    watchdog.Instance = IWDG;
    watchdog.Init.Prescaler = IWDG_PRESCALER_256;
    watchdog.Init.Reload = 511;
    watchdog_ready = HAL_IWDG_Init(&watchdog) == HAL_OK;
}

/* 本机界面直接复用上述测量与存储逻辑。 */
#include "scale_ui.inc"

/* 远端和板上按键调用同一测量函数。标定过程拒绝远端改皮重。 */
static bool local_calibrating(void)
{
    return ui.page == UI_CAL_COUNT || ui.page == UI_CAL_EMPTY || ui.page == UI_CAL_LOAD ||
        (ui.page == UI_EDIT && ui.edit_kind == EDIT_MASS);
}
const char *scale_remote_tare(uint32_t now)
{
    if (local_calibrating()) { return "ERR,BUSY\r\n"; }
    idle_since_ms = now; if (sleeping) { wake_scale(now); }
    const char *result = set_tare(now); ui_result(result, now); return result;
}
void scale_remote_wake(uint32_t now) { wake_scale(now); idle_since_ms = now; }

/* 主循环交替处理采样、按键、屏幕与蜂鸣器，菜单编辑期间继续采样。 */
int main(void)
{
    HAL_Init();
    board_init();
    scale_config_load(&saved_config);
    battery_configure(saved_config.battery_profile, saved_config.battery_reference_mv);
    lcd_set_missing_mask(saved_config.lcd_missing_mask);
    stable_counts = saved_config.stable_counts;
    buzzer_mode = (BuzzerMode)saved_config.buzzer_mode;
    zero_counts = saved_config.zero_counts;
    if (saved_config.point_count != 0U) {
        unsigned last = saved_config.point_count - 1U;
        reference_grams = saved_config.grams[last];
        zero_set = true;
        empty_reference_known = true; startup_zero_pending = true;
    }
    started_ms = HAL_GetTick();
    last_sample_ms = started_ms;
    ui.startup_ms = started_ms;
    display_interval_ms = saved_config.display_interval_ms;
    lcd_init();
    battery_init(started_ms);
    ble_configure(saved_config.bluetooth_enabled != 0U);
    uint32_t led_ms = started_ms;
    uint32_t power_ms = started_ms;
    idle_since_ms = started_ms;
    /* 独立看门狗由 LSI 驱动，约 4 秒未回到主循环时复位，实际时长受 LSI 误差影响。 */
    watchdog_start();

    for (;;) {
        uint32_t now = HAL_GetTick();
        if (watchdog_ready) {
            HAL_IWDG_Refresh(&watchdog);
        }
        battery_poll(now, sleeping);
        sample_poll(now);
        key_poll(now);
        if ((uint32_t)(now - power_ms) >= 10U) {
            power_ms = now;
            weight_update(now);
            if (ui.page == UI_WEIGH) { power_poll(now); alarm_poll(now); }
            else { idle_since_ms = now; empty_candidate = false; }
        }
        buzzer_poll(now);
        if (!sleeping && (uint32_t)(now - led_ms) >= 500U) {
            led_ms = now;
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        }
        ui_render(now);
        if (local_calibrating()) { weight_state.status = WEIGHT_CALIBRATING; weight_state.stable = false; }
        ble_poll(now, &weight_state, display_interval_ms);
        if (sleeping) {
            __WFI();
        }
    }
}
