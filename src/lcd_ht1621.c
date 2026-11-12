/*
 * HT1621B 写接口与 GDC0689 段码编码。命令/地址高位先发，显示数据 D0..D3 先发。
 * 映射依据 GDC0689 的 COM/SEG 表，并按本板 U9 实际引脚网络换算。
 * 玻璃上方四位用于菜单/状态，其余六位按逻辑顺序用于重量与参数。全部数据使用整数。
 */
#include "lcd_ht1621.h"
#include "py32f0xx_hal.h"
#include <string.h>

static uint8_t frame[32];
static uint8_t sent[32];
static bool enabled;

/* 仅保存最近一次实物读回，供调试器读取；不是发送缓存。 */
volatile uint8_t lcd_ram_readback[32];
volatile uint32_t lcd_ram_mismatch;
volatile uint8_t lcd_ram_checked;

static void edge_delay(void)
{
    for (volatile unsigned i = 0; i < 8; ++i) { __NOP(); }
}

static void write_bit(unsigned bit)
{
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, bit ? GPIO_PIN_SET : GPIO_PIN_RESET);
    edge_delay();
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
    edge_delay();
}

static void write_msb(unsigned value, unsigned count)
{
    while (count != 0U) { write_bit((value >> --count) & 1U); }
}

static void command(unsigned value)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
    write_msb(4U, 3);                  /* 100：命令模式。 */
    write_msb(value << 1, 9);          /* 8 位命令后附加 don't-care 位。 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
    edge_delay();
}

static void write_ram(unsigned address, uint8_t bits)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
    write_msb(5U, 3);                  /* 101：RAM 写入模式。 */
    write_msb(address, 6);
    for (unsigned bit = 0; bit < 4; ++bit) { write_bit((bits >> bit) & 1U); }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
    edge_delay();
}

void lcd_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4 | GPIO_PIN_5, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_1, GPIO_PIN_SET);
    GPIO_InitTypeDef pin = {.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_7,
        .Mode = GPIO_MODE_OUTPUT_PP, .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW};
    HAL_GPIO_Init(GPIOA, &pin);
    pin.Pin = GPIO_PIN_2; HAL_GPIO_Init(GPIOB, &pin);
    pin.Pin = GPIO_PIN_1; HAL_GPIO_Init(GPIOF, &pin);
    command(0x00);                     /* 先关闭振荡器和 LCD。 */
    command(0x18);                     /* 内部 256kHz RC。 */
    command(0x29);                     /* 1/3 bias，4 COM（1/4 duty）。 */
    command(0x05);                     /* 不启用 HT1621 自带看门狗输出。 */
    command(0x08);                     /* 板上蜂鸣器由 PA1 控制。 */
    command(0x01);
    memset(frame, 0, sizeof(frame));
    memset(sent, 0xff, sizeof(sent));
    lcd_ram_checked = 0;
    enabled = true;
    lcd_flush();
    command(0x03);
}

void lcd_power(bool on)
{
    if (on == enabled) { return; }
    enabled = on;
    command(on ? 0x01U : 0x02U);
    if (on) { command(0x03); memset(sent, 0xff, sizeof(sent)); }
    else { command(0x00); }
}

void lcd_clear(void) { memset(frame, 0, sizeof(frame)); }

/* a,b,c,d,e,f,g 对应 bit0..bit6；字母按七段屏可表达的形状显示。 */
static uint8_t glyph(char c)
{
    static const uint8_t digits[] = {0x3f,0x06,0x5b,0x4f,0x66,0x6d,0x7d,0x07,0x7f,0x6f};
    if (c >= '0' && c <= '9') { return digits[(unsigned)(c - '0')]; }
    switch (c) {
    case 'A': case 'a': return 0x77;
    case 'B': case 'b': return 0x7c;
    case 'C': return 0x39; case 'c': return 0x58;
    case 'D': case 'd': return 0x5e;
    case 'E': case 'e': return 0x79;
    case 'F': case 'f': return 0x71;
    case 'G': case 'g': return 0x3d;
    case 'H': case 'K': case 'k': return 0x76; case 'h': return 0x74;
    case 'I': case 'i': return 0x06;
    case 'J': case 'j': return 0x1e;
    case 'L': case 'l': return 0x38;
    case 'M': case 'm': return 0x37;
    case 'N': case 'n': return 0x54;
    case 'O': return 0x3f; case 'o': return 0x5c;
    case 'P': case 'p': return 0x73;
    case 'R': case 'r': return 0x50;
    case 'S': case 's': return 0x6d;
    case 'T': case 't': return 0x78;
    case 'U': return 0x3e; case 'u': return 0x1c;
    case 'V': case 'v': return 0x3e;
    case 'Y': case 'y': return 0x6e;
    case 'Z': case 'z': return 0x5b;
    case 'W': case 'w': return 0x2a;
    case '-': return 0x40;
    default: return 0;
    }
}

static void digit(unsigned position, char c, bool dot)
{
    uint8_t g = glyph(c);
    if (position < 4U) {
        unsigned address = position * 2U;
        frame[address] = ((g >> 3) & 1U) | (((g >> 4) & 1U) << 1) |
            (((g >> 6) & 1U) << 2) | (((g >> 5) & 1U) << 3);
        /* 实机逐地址确认：第四位的 A/B/C 在 SEG7，而非旧表的 SEG22。 */
        unsigned second = address + 1U;
        frame[second] = (position < 3U && dot ? 1U : 0U) |
            (((g >> 2) & 1U) << 1) | (((g >> 1) & 1U) << 2) | ((g & 1U) << 3);
    } else {
        static const uint8_t address[] = {8,10,12,16,18,20};
        unsigned a = address[position - 4U];
        frame[a] = ((g >> 5) & 1U) | (((g >> 6) & 1U) << 1) |
            (((g >> 4) & 1U) << 2) | (((g >> 3) & 1U) << 3);
        frame[a + 1U] = (g & 1U) | (((g >> 1) & 1U) << 1) |
            (((g >> 2) & 1U) << 2) | (position < 9U && dot ? 8U : 0U);
    }
}

void lcd_text(const char *title, const char *value, uint8_t dots)
{
    lcd_clear();
    bool ended = false;
    for (unsigned i = 0; i < 4; ++i) {
        char c = ended ? ' ' : title[i];
        if (c == '\0') { ended = true; c = ' '; }
        digit(i, c, false);
    }
    ended = false;
    for (unsigned i = 0; i < 6; ++i) {
        char c = ended ? ' ' : value[i];
        if (c == '\0') { ended = true; c = ' '; }
        digit(i + 4U, c, (dots & (1U << i)) != 0U);
    }
}

void lcd_number(const char *title, int32_t value, unsigned decimals)
{
    if (decimals > 5U) { lcd_text(title, "------", 0); return; }
    char text[7] = "      ";
    bool negative = value < 0;
    uint32_t magnitude = negative ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
    unsigned at = 6;
    unsigned used = 0;
    do {
        if (at == 0U) { lcd_text(title, "------", 0); return; }
        text[--at] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
        ++used;
    } while (magnitude != 0U || used <= decimals);
    if (negative) {
        if (at == 0U) { lcd_text(title, "------", 0); return; }
        text[--at] = '-';
    }
    lcd_text(title, text, decimals != 0U ? (uint8_t)(1U << (5U - decimals)) : 0U);
}

void lcd_weight(int32_t mg, bool kg, bool stable, bool tare)
{
    char title[5] = {kg ? 'k' : 'g', kg ? 'g' : ' ', tare ? 't' : ' ', stable ? 'S' : ' ', 0};
    int32_t tenths = mg < 0 ? -(int32_t)((-(int64_t)mg + 50) / 100) : (mg + 50) / 100;
    lcd_number(title, tenths, kg ? 4U : 1U);
}

void lcd_test_all(void) { memset(frame, 0x0f, sizeof(frame)); }

/* 从 HT1621 本体读取32个RAM单元。RD下降沿推出数据，随后上升沿后采样。
 * 先将PA7切为输入，CS释放后再恢复推挽，避免与HT1621的读输出争用。
 */
bool lcd_verify_ram(void)
{
    if (!enabled) { lcd_ram_checked = 0; return false; }
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_1, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_RESET);
    edge_delay();
    write_msb(6U, 3);                /* 110：读RAM。 */
    write_msb(0U, 6);                /* 从地址0连续读取。 */
    GPIO_InitTypeDef pin = {.Pin = GPIO_PIN_7, .Mode = GPIO_MODE_INPUT,
        .Pull = GPIO_NOPULL, .Speed = GPIO_SPEED_FREQ_LOW};
    HAL_GPIO_Init(GPIOA, &pin);
    uint32_t mismatch = 0;
    for (unsigned address = 0; address < 32; ++address) {
        unsigned data = 0;
        for (unsigned bit = 0; bit < 4; ++bit) {
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_1, GPIO_PIN_RESET);
            edge_delay(); edge_delay();
            HAL_GPIO_WritePin(GPIOF, GPIO_PIN_1, GPIO_PIN_SET);
            edge_delay(); edge_delay();
            if ((GPIOA->IDR & GPIO_PIN_7) != 0U) { data |= 1U << bit; }
        }
        lcd_ram_readback[address] = (uint8_t)data;
        if (data != frame[address]) { mismatch |= 1UL << address; }
    }
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_2, GPIO_PIN_SET);
    edge_delay();
    pin.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOA, &pin);
    lcd_ram_mismatch = mismatch;
    lcd_ram_checked = 1;
    return mismatch == 0U;
}

unsigned lcd_ram_result(void)
{
    return lcd_ram_checked ? (lcd_ram_mismatch == 0U ? 0U : 1U) : 2U;
}

void lcd_flush(void)
{
    if (!enabled) { return; }
    for (unsigned i = 0; i < 32; ++i) {
        if (frame[i] != sent[i]) { write_ram(i, frame[i]); sent[i] = frame[i]; }
    }
}

#ifdef SCALE_HOST_TEST
const uint8_t *lcd_test_frame(void) { return frame; }
#endif
