/*
 * 电子秤基础固件，入口对应 PlatformIO 的 scale_basic 环境。
 * 数据流为 HX711 原始计数 → 滤波与稳定判定 → 校准换算 → 去皮 → 串口上报。
 * 标定、单位、蜂鸣器、判稳和待机设置通过双页 CRC 记录掉电保存。
 * 时序均由毫秒计时调度，采样、按键、蜂鸣器和串口可以交替处理。
 */
#include "py32f0xx_hal.h"
#include "scale_config.h"
#include "weight_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 引脚来自本板原理图。PA13/PA14 由 SWD 使用，PF0/PF1 留给显示接口。 */
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
    DEFAULT_STABLE_COUNTS = 64,  /* 最近 8 点的最大值与最小值之差。 */
    DEFAULT_REPORT_MS = 200,       /* 文档要求 5～10Hz，默认选择 5Hz。 */
    MIN_REPORT_MS = 100,
    MAX_REPORT_MS = 200,
    KEY_DEBOUNCE_MS = 30,
    KEY_LONG_PRESS_MS = 1500,
    BUZZER_ON_MS = 120,
    BUZZER_GAP_MS = 100,
    ALARM_INTERVAL_MS = 2000,
    UART_BAUD = 9600,
    MAX_CONFIG_VALUE = 1000000,
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

/* 运行时空载与去皮偏移。标定的各段计数允许为负，适配输出方向相反的传感器。 */
static int32_t zero_counts;
static int32_t tare_counts;
static uint32_t reference_grams;
static bool zero_set;

/* 蜂鸣器使用分段计时，响声之间继续运行采样和串口。 */
static BuzzerMode buzzer_mode = BUZZER_OFF;
static uint8_t buzzer_remaining;
static bool buzzer_on;
static uint32_t buzzer_phase_ms;
static uint32_t buzzer_tone_ms;

/* 串口使用 ASCII 行协议，命令以 CR 或 LF 结束。收到回复后再发下一条。 */
static char rx_line[48];
static uint8_t rx_length;
static bool rx_ready;
static bool rx_invalid;
static char tx_line[192];
static const char *tx_next;
static bool stream_enabled = true;
static uint32_t report_interval_ms = DEFAULT_REPORT_MS;

/* 文档 V1.0 的持久配置和统一状态。电池信息在硬件换算确认前使用未知值。 */
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
static bool tare_sequence_seen;
static uint32_t last_tare_sequence;
static char last_tare_reply[80];
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
    __HAL_RCC_USART1_CLK_ENABLE();

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

    /* PA2 为 TX，PA3 为 RX，AF1。按 ECB01C 出厂设置使用 9600、8N1。 */
    USART1->CR1 = 0;
    USART1->CR2 = 0;
    USART1->CR3 = 0;
    USART1->BRR = (HAL_RCC_GetPCLK1Freq() + UART_BAUD / 2U) / UART_BAUD;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
    pin.Mode = GPIO_MODE_AF_PP;
    pin.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    pin.Pull = GPIO_NOPULL;
    pin.Alternate = GPIO_AF1_USART1;
    HAL_GPIO_Init(GPIOA, &pin);
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

/* 收入一个有效读数，并用完整窗口的峰峰值判断秤台是否持续静止。 */
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
    if (sample_count < AVERAGE_SAMPLES || (uint32_t)(maximum - minimum) > stable_counts) {
        settling = false;
        stable = false;
        return;
    }
    if (!settling) {
        settling = true;
        stable_since_ms = now;
    }
    stable = (uint32_t)(now - stable_since_ms) >= STABLE_HOLD_MS;
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
    saved_config = *candidate;
    stable_counts = candidate->stable_counts;
    buzzer_mode = (BuzzerMode)candidate->buzzer_mode;
    return true;
}

/* 按毛重检查上限，容器去皮后依然计入传感器承受的负载。 */
static bool overloaded(uint32_t now)
{
    return reference_grams != 0U && samples_ready(now) &&
           gross_milligrams(filtered_counts()) > (int64_t)MAX_GROSS_GRAMS * 1000;
}

/* 检查测量操作的共同前提，并给串口返回可定位的问题。NULL 表示允许执行。 */
static const char *measurement_error(uint32_t now, bool check_capacity)
{
    if (!samples_ready(now)) {
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
    buzzer_tone_ms = now;
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
        buzzer_tone_ms = now;
        if (buzzer_on) {
            --buzzer_remaining;
            buzzer_on = false;
            HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
        } else {
            buzzer_on = true;
            HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
        }
    } else if (buzzer_on && buzzer_mode == BUZZER_PASSIVE &&
               (uint32_t)(now - buzzer_tone_ms) >= 1U) {
        buzzer_tone_ms = now;
        HAL_GPIO_TogglePin(BUZZER_PORT, BUZZER_PIN);
    }
}

/* SW4 按下为低。短按在释放时去皮，长按到 1.5 秒归零，每次长按只触发一次。 */
static void key_poll(uint32_t now)
{
    static bool last_pressed;
    static bool debounced_pressed;
    static bool long_handled;
    static uint32_t changed_ms;
    static uint32_t pressed_ms;
    const char *result = NULL;
    bool pressed = (KEY_PORT->IDR & KEY_PIN) == 0U;
    if (pressed) { idle_since_ms = now; }

    if (pressed != last_pressed) {
        last_pressed = pressed;
        changed_ms = now;
    }
    if (pressed != debounced_pressed && (uint32_t)(now - changed_ms) >= KEY_DEBOUNCE_MS) {
        debounced_pressed = pressed;
        if (pressed) {
            pressed_ms = now;
            long_handled = false;
            if (sleeping) {
                wake_scale(now);
                long_handled = true; /* 唤醒用的这次按键在释放前只做唤醒。 */
            }
        } else if (!long_handled) {
            result = set_tare(now);
        }
    }
    if (debounced_pressed && pressed && !long_handled &&
        (uint32_t)(now - pressed_ms) >= KEY_LONG_PRESS_MS) {
        long_handled = true;
        result = set_zero(now);
    }
    if (result != NULL) {
        buzzer_start(result[0] == 'O' ? 1U : 3U, now);
    }
}

/* 严格读取非负整数，空串、符号、单位、尾随空格和超大数字均返回失败。 */
static bool parse_value(const char *text, uint32_t *result)
{
    if (*text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (MAX_CONFIG_VALUE - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (*text != '\0') {
        return false;
    }
    *result = value;
    return true;
}

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
    weight_state.stable = false;
    if (sleeping) {
        weight_state.status = WEIGHT_SLEEP;
        weight_state.motion = MOTION_SLEEP;
        return;
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

/* 将毫克格式化为一位小数的克数。蓝牙统一使用 g，显示端可按 unit 转换单位。 */
static void format_grams(char *output, size_t capacity, int32_t milligrams)
{
    uint32_t magnitude = milligrams < 0 ? (uint32_t)(-(int64_t)milligrams) : (uint32_t)milligrams;
    uint32_t decigrams = (magnitude + 50U) / 100U;
    snprintf(output, capacity, "%s%lu.%lu", milligrams < 0 && decigrams != 0U ? "-" : "",
             (unsigned long)(decigrams / 10U), (unsigned long)(decigrams % 10U));
}

/* 文档帧 W,序号,净重g,稳定,皮重g,电量百分比,状态。错误时重量字段留空。 */
static const char *weight_report(uint32_t now)
{
    static const char *const statuses[] = {"WAIT", "UNCAL", "OK", "RANGE", "OL", "ERR", "SLEEP"};
    weight_update(now);
    ++weight_state.sequence;
    if (weight_state.status != WEIGHT_OK) {
        snprintf(tx_line, sizeof(tx_line), "W,%lu,,0,,%d,%s\r\n",
                 (unsigned long)weight_state.sequence, (int)weight_state.battery_percent,
                 statuses[weight_state.status]);
        return tx_line;
    }
    char net[20];
    char tare[20];
    format_grams(net, sizeof(net), weight_state.display_mg);
    format_grams(tare, sizeof(tare), weight_state.tare_mg);
    snprintf(tx_line, sizeof(tx_line), "W,%lu,%s,%u,%s,%d,OK\r\n",
             (unsigned long)weight_state.sequence, net, (unsigned)weight_state.stable,
             tare, (int)weight_state.battery_percent);
    return tx_line;
}

/* 状态查询保留调试字段，重量报文只携带平板所需的统一状态。 */
static const char *status_report(uint32_t now)
{
    static const char *const modes[] = {"OFF", "ACTIVE", "PASSIVE"};
    weight_update(now);
    snprintf(tx_line, sizeof(tx_line),
             "S,READY=%u,STABLE=%u,CAL_POINTS=%lu,TARE_COUNTS=%ld,STATUS=%u,"
             "DIV_MG=%u,EMPTY=%u,AUTO_OFF_S=%lu,SLEEP=%u,BUZZER=%s,UNIT=%s\r\n",
             (unsigned)samples_ready(now), (unsigned)weight_state.stable,
             (unsigned long)saved_config.point_count, (long)tare_counts,
             (unsigned)weight_state.status, (unsigned)weight_state.division_mg,
             (unsigned)empty_candidate, (unsigned long)saved_config.auto_off_seconds,
             (unsigned)sleeping, modes[buzzer_mode], saved_config.unit == 0U ? "g" : "kg");
    return tx_line;
}

/* 去皮命令序号使用完整 uint32 范围，解析前检查溢出。 */
static bool parse_sequence(const char *text, uint32_t *result)
{
    if (*text == '\0') {
        return false;
    }
    uint32_t value = 0;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *result = value;
    return *text == '\0';
}

/* 重试最近序号重发原应答，更早的序号拒绝执行，防止断线重传重复去皮。 */
static const char *sequenced_tare(uint32_t now)
{
    uint32_t sequence;
    if (!parse_sequence(rx_line + 5, &sequence)) {
        buzzer_start(3, now);
        return "ERR,TARE,?,BAD_SEQ\r\n";
    }
    if (tare_sequence_seen && sequence == last_tare_sequence) {
        return last_tare_reply;
    }
    if (tare_sequence_seen && (uint32_t)(sequence - last_tare_sequence) >= 0x80000000U) {
        snprintf(tx_line, sizeof(tx_line), "ERR,TARE,%lu,STALE_SEQ\r\n", (unsigned long)sequence);
        return tx_line;
    }
    const char *result = set_tare(now);
    if (result[0] == 'O') {
        snprintf(last_tare_reply, sizeof(last_tare_reply), "ACK,TARE,%lu\r\n", (unsigned long)sequence);
        buzzer_start(1, now);
    } else {
        snprintf(last_tare_reply, sizeof(last_tare_reply), "ERR,TARE,%lu,%.*s\r\n",
                 (unsigned long)sequence, (int)strlen(result) - 6, result + 4);
        buzzer_start(3, now);
    }
    last_tare_sequence = sequence;
    tare_sequence_seen = true;
    return last_tare_reply;
}

/* 标定点按砝码质量递增录入，同一质量重标时废弃后续点，保留单调有效曲线。 */
static const char *calibrate(uint32_t now)
{
    uint32_t grams;
    if (!parse_value(rx_line + 4, &grams) || grams == 0U || grams > MAX_GROSS_GRAMS || !zero_set) {
        return "ERR,CAL_NEEDS_ZERO_1_TO_5000G\r\n";
    }
    /* 标定依据已确认的砝码质量，旧曲线的偏差不能阻止满量程标定。 */
    const char *error = measurement_error(now, false);
    if (error != NULL) {
        return error;
    }
    ScaleConfig candidate = saved_config;
    unsigned index = 0;
    while (index < candidate.point_count && candidate.grams[index] < grams) {
        ++index;
    }
    if (index == 3U) {
        return "ERR,CAL_MAX_3_POINTS\r\n";
    }
    candidate.zero_counts = zero_counts;
    candidate.spans[index] = average_counts() - zero_counts;
    candidate.grams[index] = grams;
    candidate.point_count = index + 1U;
    if (!scale_config_valid(&candidate)) {
        return "ERR,CAL_SPAN_OR_ORDER\r\n";
    }
    if (!save_settings(&candidate)) {
        return "ERR,FLASH\r\n";
    }
    reference_grams = grams;
    tare_counts = 0;
    weight_update(now);
    return "OK,CAL,SAVED\r\n";
}

/* 分发控制与维护命令。持久设置先验证并保存成功，再应用到运行状态。 */
static const char *handle_command(uint32_t now)
{
    if (rx_invalid) {
        return "ERR,COMMAND\r\n";
    }
    if (strcmp(rx_line, "HELP") == 0) {
        return "CMDS,ZERO,CAL <g>,CALRESET,TARE[,seq],UNTARE,READ,RAW,STATUS,INFO,"
               "RATE <100..200>,STREAM <0|1>,BUZZER <OFF|ACTIVE|PASSIVE>,"
               "UNIT <g|kg>,AUTO_OFF <0..86400>,STABLE <counts>,DEVICE <id>,CALDATE <YYYYMMDD>,WAKE\r\n";
    }
    if (strcmp(rx_line, "READ") == 0) {
        return weight_report(now);
    }
    if (strcmp(rx_line, "RAW") == 0) {
        if (!samples_ready(now)) {
            return "ERR,WAIT_SAMPLES\r\n";
        }
        snprintf(tx_line, sizeof(tx_line), "RAW,%ld\r\n", (long)latest_raw);
        return tx_line;
    }
    if (strcmp(rx_line, "STATUS") == 0) {
        return status_report(now);
    }
    if (strcmp(rx_line, "INFO") == 0) {
        snprintf(tx_line, sizeof(tx_line), "INFO,FW=1.0.2,ID=%s,CAL_DATE=%lu,PROTO=2,CAP_G=5000,OL_G=5250,BAT=-1,WDG=%u\r\n",
                 saved_config.device_id, (unsigned long)saved_config.calibration_date, (unsigned)watchdog_ready);
        return tx_line;
    }
    if (strncmp(rx_line, "TARE,", 5) == 0) {
        return sequenced_tare(now);
    }
    if (strcmp(rx_line, "UNTARE") == 0) {
        tare_counts = 0;
        weight_update(now);
        return "OK,UNTARE\r\n";
    }
    if (strcmp(rx_line, "TARE") == 0) {
        return set_tare(now);
    }
    if (strcmp(rx_line, "ZERO") == 0) {
        return set_zero(now);
    }
    if (strcmp(rx_line, "CALRESET") == 0) {
        ScaleConfig candidate = saved_config;
        candidate.zero_counts = 0;
        candidate.point_count = 0;
        memset(candidate.spans, 0, sizeof(candidate.spans));
        memset(candidate.grams, 0, sizeof(candidate.grams));
        if (!save_settings(&candidate)) {
            return "ERR,FLASH\r\n";
        }
        zero_counts = 0;
        reference_grams = 0;
        tare_counts = 0;
        zero_set = false;
        empty_reference_known = false;
        weight_update(now);
        return "OK,CALRESET\r\n";
    }
    if (strncmp(rx_line, "CAL ", 4) == 0) {
        return calibrate(now);
    }
    if (strncmp(rx_line, "LIMIT ", 6) == 0) {
        return strcmp(rx_line, "LIMIT 5000") == 0 ? "OK,LIMIT\r\n" : "ERR,LIMIT_FIXED_5000\r\n";
    }
    if (strncmp(rx_line, "STABLE ", 7) == 0) {
        uint32_t value;
        if (!parse_value(rx_line + 7, &value) || value == 0U) {
            return "ERR,STABLE_COUNTS\r\n";
        }
        ScaleConfig candidate = saved_config;
        candidate.stable_counts = value;
        if (!save_settings(&candidate)) {
            return "ERR,FLASH\r\n";
        }
        sample_reset();
        return "OK,STABLE\r\n";
    }
    if (strncmp(rx_line, "RATE ", 5) == 0) {
        uint32_t value;
        if (!parse_value(rx_line + 5, &value) || value < MIN_REPORT_MS || value > MAX_REPORT_MS) {
            return "ERR,RATE_100_TO_200_MS\r\n";
        }
        report_interval_ms = value;
        return "OK,RATE\r\n";
    }
    if (strcmp(rx_line, "STREAM 0") == 0 || strcmp(rx_line, "STREAM 1") == 0) {
        stream_enabled = rx_line[7] == '1';
        return "OK,STREAM\r\n";
    }
    if (strncmp(rx_line, "BUZZER ", 7) == 0) {
        const char *mode = rx_line + 7;
        ScaleConfig candidate = saved_config;
        if (strcmp(mode, "OFF") == 0) {
            candidate.buzzer_mode = BUZZER_OFF;
        } else if (strcmp(mode, "ACTIVE") == 0) {
            candidate.buzzer_mode = BUZZER_ACTIVE;
        } else if (strcmp(mode, "PASSIVE") == 0) {
            candidate.buzzer_mode = BUZZER_PASSIVE;
        } else {
            return "ERR,BUZZER_MODE\r\n";
        }
        if (!save_settings(&candidate)) {
            return "ERR,FLASH\r\n";
        }
        buzzer_remaining = 0;
        buzzer_on = false;
        HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
        buzzer_start(1, now);
        return "OK,BUZZER\r\n";
    }
    if (strncmp(rx_line, "AUTO_OFF ", 9) == 0) {
        uint32_t seconds;
        if (!parse_value(rx_line + 9, &seconds) || seconds > 86400U) {
            return "ERR,AUTO_OFF_SECONDS\r\n";
        }
        ScaleConfig candidate = saved_config;
        candidate.auto_off_seconds = seconds;
        return save_settings(&candidate) ? "OK,AUTO_OFF\r\n" : "ERR,FLASH\r\n";
    }
    if (strncmp(rx_line, "UNIT ", 5) == 0) {
        ScaleConfig candidate = saved_config;
        if (strcmp(rx_line + 5, "g") == 0) {
            candidate.unit = 0;
        } else if (strcmp(rx_line + 5, "kg") == 0) {
            candidate.unit = 1;
        } else {
            return "ERR,UNIT\r\n";
        }
        return save_settings(&candidate) ? "OK,UNIT\r\n" : "ERR,FLASH\r\n";
    }
    if (strncmp(rx_line, "DEVICE ", 7) == 0) {
        const char *id = rx_line + 7;
        size_t length = strlen(id);
        if (length == 0 || length >= sizeof(saved_config.device_id)) {
            return "ERR,DEVICE_ID\r\n";
        }
        for (size_t i = 0; i < length; ++i) {
            if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'Z') ||
                  (id[i] >= 'a' && id[i] <= 'z') || id[i] == '-' || id[i] == '_')) {
                return "ERR,DEVICE_ID\r\n";
            }
        }
        ScaleConfig candidate = saved_config;
        memset(candidate.device_id, 0, sizeof(candidate.device_id));
        memcpy(candidate.device_id, id, length);
        return save_settings(&candidate) ? "OK,DEVICE\r\n" : "ERR,FLASH\r\n";
    }
    if (strncmp(rx_line, "CALDATE ", 8) == 0) {
        uint32_t date;
        if (!parse_sequence(rx_line + 8, &date)) {
            return "ERR,CALDATE\r\n";
        }
        unsigned year = date / 10000U;
        unsigned month = date / 100U % 100U;
        unsigned day = date % 100U;
        static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        bool leap = year % 4U == 0 && (year % 100U != 0 || year % 400U == 0);
        if (year < 2000U || year > 9999U || month < 1U || month > 12U || day < 1U ||
            day > days[month - 1U] + (unsigned)(month == 2U && leap)) {
            return "ERR,CALDATE\r\n";
        }
        ScaleConfig candidate = saved_config;
        candidate.calibration_date = date;
        return save_settings(&candidate) ? "OK,CALDATE\r\n" : "ERR,FLASH\r\n";
    }
    if (strcmp(rx_line, "WAKE") == 0) {
        wake_scale(now);
        return "OK,WAKE\r\n";
    }
    return "ERR,COMMAND\r\n";
}

/* 每轮最多收发一个字节。接收错误或超长行一直作废到行尾，防止误执行截断命令。 */
static void uart_poll(uint32_t now)
{
    uint32_t status = USART1->SR;
    const uint32_t errors = USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE;
    if ((status & (USART_SR_RXNE | errors)) != 0U) {
        idle_since_ms = now;
        if (sleeping) { wake_scale(now); }
        uint8_t byte = (uint8_t)USART1->DR; /* 读 SR 后读 DR，清除接收错误标志。 */
        if (!rx_ready) {
            if ((status & errors) != 0U) {
                rx_invalid = true;
            } else if (byte == '\r' || byte == '\n') {
                if (rx_length != 0U || rx_invalid) {
                    rx_line[rx_length] = '\0';
                    rx_ready = true;
                }
            } else if (byte < 32U || byte > 126U || rx_length == sizeof(rx_line) - 1U) {
                rx_invalid = true;
            } else if (!rx_invalid) {
                rx_line[rx_length++] = (char)byte;
            }
        }
    }
    if (tx_next == NULL && rx_ready) {
        tx_next = handle_command(now);
        /* 带序号去皮在去重分支内提示。其他命令成功一声、失败三声，查询结果保持安静。 */
        if (strncmp(rx_line, "TARE,", 5) != 0) {
            if (strncmp(tx_next, "ERR,", 4) == 0) {
                buzzer_start(3, now);
            } else if (strncmp(tx_next, "OK,", 3) == 0) {
                buzzer_start(1, now);
            }
        }
        rx_length = 0;
        rx_ready = false;
        rx_invalid = false;
    }
    if (tx_next != NULL && (status & USART_SR_TXE) != 0U) {
        USART1->DR = (uint8_t)*tx_next++;
        if (*tx_next == '\0') {
            tx_next = NULL;
        }
    }
}

/* 软件待机使 HX711 掉电并熄灭 LED，主控保留串口、按键和毫秒唤醒。 */
static void enter_standby(void)
{
    sleeping = true;
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
                 weight_state.status == WEIGHT_OVERLOAD || weight_state.status == WEIGHT_RANGE);
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

/* 主循环按顺序轮询各外设。LED 心跳与串口上报分开计时，关掉上报仍能看见运行状态。 */
int main(void)
{
    HAL_Init();
    board_init();
    scale_config_load(&saved_config);
    stable_counts = saved_config.stable_counts;
    buzzer_mode = (BuzzerMode)saved_config.buzzer_mode;
    zero_counts = saved_config.zero_counts;
    if (saved_config.point_count != 0U) {
        unsigned last = saved_config.point_count - 1U;
        reference_grams = saved_config.grams[last];
        zero_set = true;
        empty_reference_known = true;
    }
    started_ms = HAL_GetTick();
    last_sample_ms = started_ms;
    uint32_t report_ms = started_ms;
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
        sample_poll(now);
        key_poll(now);
        uart_poll(now);
        if ((uint32_t)(now - power_ms) >= 10U) {
            power_ms = now;
            power_poll(now);
            alarm_poll(now);
        }
        buzzer_poll(now);
        if (!sleeping && (uint32_t)(now - led_ms) >= 500U) {
            led_ms = now;
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        }
        if (stream_enabled && (uint32_t)(now - report_ms) >= report_interval_ms && tx_next == NULL) {
            report_ms = now;
            tx_next = weight_report(now);
        }
        if (sleeping && tx_next == NULL) {
            __WFI();
        }
    }
}
