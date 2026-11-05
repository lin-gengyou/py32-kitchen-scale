/* 直接包含固件实现，模拟 ADC 输入、UART 字节和两页 Flash 的写入中断。 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "py32f0xx_hal.h"

static uint32_t host_flash[64];
static int flash_write_bytes = 128;
static bool erase_failure;
static bool program_late_error;
static unsigned flash_program_count;
FLASH_ProcessTypeDef pFlash;
HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error)
{
    if (erase_failure) { return HAL_ERROR; }
    assert(erase->PageAddress >= (uintptr_t)host_flash);
    assert(erase->PageAddress + 128 <= (uintptr_t)host_flash + sizeof(host_flash));
    assert(erase->NbPages == 1);
    memset((void *)erase->PageAddress, 0xff, 128);
    *error = UINT32_MAX;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uintptr_t address, uint32_t *data)
{
    assert(type == FLASH_TYPEPROGRAM_PAGE);
    assert(flash_write_bytes >= 0 && flash_write_bytes <= 128);
    ++flash_program_count;
    memcpy((void *)address, data, (size_t)flash_write_bytes);
    return flash_write_bytes == 128 && !program_late_error ? HAL_OK : HAL_ERROR;
}
#define SCALE_CONFIG_BASE ((uintptr_t)host_flash)
#include "../../src/scale_config.c"
#define main firmware_main
#include "../../src/scale_basic.c"
#undef main

GPIO_TypeDef host_gpio_a, host_gpio_b;
USART_TypeDef host_usart;

static void command(const char *input, const char *expected, uint32_t now)
{
    assert(strlen(input) < sizeof(rx_line));
    strcpy(rx_line, input);
    rx_invalid = false;
    const char *actual = handle_command(now);
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "Command %s: expected %s, got %s", input, expected, actual);
        assert(false);
    }
}

static void settle(int32_t counts, uint32_t *now)
{
    for (unsigned i = 0; i < 18; ++i) {
        *now += 100U;
        sample_add(counts, *now);
    }
    assert(stable && samples_ready(*now));
    weight_update(*now);
}

static const char *uart_request(const char *input, uint32_t now)
{
    static char response[512];
    assert(tx_next == NULL);
    while (*input != '\0') {
        USART1->SR = USART_SR_RXNE;
        USART1->DR = (uint8_t)*input++;
        uart_poll(now);
    }
    unsigned length = 0;
    while (tx_next != NULL) {
        assert(length < sizeof(response) - 1U);
        USART1->SR = USART_SR_TXE;
        uart_poll(now);
        response[length++] = (char)USART1->DR;
    }
    response[length] = '\0';
    USART1->SR = 0;
    return response;
}

static void storage_checks(void)
{
    memset(host_flash, 0xff, sizeof(host_flash));
    ScaleConfig first, recovered;
    assert(!scale_config_load(&first));
    assert(first.auto_off_seconds == 120 && first.point_count == 0);
    assert(scale_config_save(&first));
    unsigned writes = flash_program_count;
    assert(scale_config_save(&first) && flash_program_count == writes);
    ScaleConfig second = first;
    second.zero_counts = 100000;
    second.spans[0] = -500000;
    second.grams[0] = 500;
    second.point_count = 1;
    assert(scale_config_save(&second));
    assert(scale_config_load(&recovered) && recovered.point_count == 1);
    ScaleConfig third = second;
    third.unit = 1;
    for (int cut = 0; cut < (int)sizeof(ConfigRecord); ++cut) {
        flash_write_bytes = cut;
        assert(!scale_config_save(&third));
        assert(scale_config_load(&recovered));
        assert(memcmp(&recovered, &second, sizeof(second)) == 0);
    }
    flash_write_bytes = 128;
    assert(scale_config_save(&third));
    assert(scale_config_load(&recovered) && recovered.unit == 1);
    /* 新页 CRC 被损坏后，应恢复上一份完整标定。 */
    int active = current_slot();
    ConfigRecord *record = (ConfigRecord *)((uint8_t *)host_flash + active * 128);
    record->crc ^= 1;
    assert(scale_config_load(&recovered) && recovered.unit == 0 && recovered.point_count == 1);
    erase_failure = true;
    assert(!scale_config_save(&third));
    assert(scale_config_load(&recovered) && recovered.unit == 0);
    erase_failure = false;
    /* 驱动返回错误但实际整页写入完整时，以验证后的新记录作为已提交结果。 */
    program_late_error = true;
    assert(scale_config_save(&third));
    assert(scale_config_load(&recovered) && recovered.unit == 1);
    program_late_error = false;
    memset(host_flash, 0xff, sizeof(host_flash));
}

int main(void)
{
    storage_checks();
    scale_config_load(&saved_config);
    uint32_t now = 0;
    command("ZERO", "ERR,WAIT_SAMPLES\r\n", now);
    command("TARE", "ERR,NEED_CAL\r\n", now);
    settle(100000, &now);
    command("ZERO", "OK,ZERO\r\n", now);
    command("CAL 500", "ERR,CAL_SPAN_OR_ORDER\r\n", now);
    settle(-400000, &now);
    command("CAL 500", "OK,CAL,SAVED\r\n", now);
    settle(-2400000, &now);
    command("CAL 2500", "OK,CAL,SAVED\r\n", now);
    settle(-4900000, &now);
    saved_config.spans[1] = -2450000; /* 模拟初步曲线把真实 5000g 换算得偏大。 */
    assert(overloaded(now));
    command("CAL 5000", "OK,CAL,SAVED\r\n", now);
    saved_config.spans[1] = -2500000;
    assert(scale_config_save(&saved_config)); /* 后续测试恢复每计数 1mg 的线性夹具。 */
    assert(saved_config.point_count == 3 && saved_config.spans[2] == -5000000);
    ScaleConfig recovered;
    assert(scale_config_load(&recovered) && recovered.point_count == 3);
    assert(gross_milligrams(-156400) == 256400);
    assert(gross_milligrams(100500) == -500);
    /* 非线性分段标定必须分别换算毛重与皮重，再求净重。 */
    saved_config.spans[1] = -2400000;
    saved_config.spans[2] = -4900000;
    tare_counts = -500000;
    settle(-1350000, &now);
    assert(weight_state.net_mg == 1000000);
    saved_config = recovered;
    tare_counts = 0;

    /* 文档要求的小重量保持，归零不能吞掉 0.5g。 */
    settle(99500, &now);
    assert(weight_state.net_mg == 500 && weight_state.display_mg == 500);
    assert(strstr(weight_report(now), ",0.5,1,0.0,-1,OK\r\n") != NULL);
    command("ZERO", "ERR,NOT_EMPTY\r\n", now);
    assert(zero_counts == 100000);
    settle(100000, &now);
    command("ZERO", "OK,ZERO\r\n", now);
    sample_add(-8000000, ++now);
    assert(stable && filtered_counts() == 100000); /* 单个尖峰被中值滤波去掉。 */
    sample_add(100000, ++now);
    assert(filtered_counts() == 100000);

    /* 最后一步量化，且在两个边界上有小滞回。 */
    weight_state.division_mg = 100;
    assert(select_division(200000) == 100);
    assert(select_division(200600) == 500);
    weight_state.division_mg = 500;
    assert(select_division(199600) == 500);
    assert(select_division(199400) == 100);
    assert(select_division(1001100) == 1000);
    weight_state.division_mg = 1000;
    assert(select_division(999500) == 1000);
    assert(select_division(998900) == 500);
    settle(-100600, &now);
    assert(weight_state.display_mg == 200500);

    /* 去皮后的总负载仍受 5kg 限制，5250g 为单独的 OL 门限。 */
    settle(-2900000, &now); /* 毛重 3000g。 */
    command("TARE", "OK,TARE\r\n", now);
    assert(sample_count == FAST_SAMPLES && weight_state.net_mg == 0);
    settle(-4901000, &now);
    assert(weight_state.status == WEIGHT_RANGE);
    assert(strstr(weight_report(now), ",,0,,-1,RANGE\r\n") != NULL);
    command("TARE", "ERR,OVERLOAD\r\n", now);
    settle(-5150000, &now);
    assert(weight_state.status == WEIGHT_OVERLOAD);
    assert(strstr(weight_report(now), ",,0,,-1,OL\r\n") != NULL);
    settle(100000, &now);
    assert(weight_state.net_mg == -3000000 && weight_state.display_mg == -3000000);
    command("UNTARE", "OK,UNTARE\r\n", now);
    command("LIMIT 0", "ERR,LIMIT_FIXED_5000\r\n", now);
    command("RATE 200", "OK,RATE\r\n", now);
    command("RATE 201", "ERR,RATE_100_TO_200_MS\r\n", now);

    /* 相同去皮序号重复应答，不再次归零，更早序号拒绝执行。 */
    settle(-20000, &now);
    command("TARE,17", "ACK,TARE,17\r\n", now);
    int32_t first_tare = tare_counts;
    settle(-156400, &now);
    assert(weight_state.net_mg == 136400);
    command("TARE,17", "ACK,TARE,17\r\n", now);
    assert(tare_counts == first_tare);
    command("TARE,16", "ERR,TARE,16,STALE_SEQ\r\n", now);
    command("TARE,18", "ACK,TARE,18\r\n", now);
    command("TARE,19", "ERR,TARE,19,UNSTABLE\r\n", now);
    settle(-156400, &now);
    command("TARE,19", "ERR,TARE,19,UNSTABLE\r\n", now);
    assert(tare_counts == -256400);
    uint32_t sequence;
    assert(parse_sequence("4294967295", &sequence) && sequence == UINT32_MAX);
    assert(!parse_sequence("4294967296", &sequence));
    assert(!parse_sequence("", &sequence));

    /* 参数失败不修改内存或已保存记录，日期校验支持闰年。 */
    command("AUTO_OFF 120", "OK,AUTO_OFF\r\n", now);
    command("UNIT kg", "OK,UNIT\r\n", now);
    command("DEVICE SCALE-001", "OK,DEVICE\r\n", now);
    command("CALDATE 20240229", "OK,CALDATE\r\n", now);
    command("CALDATE 20260229", "ERR,CALDATE\r\n", now);
    assert(saved_config.calibration_date == 20240229);
    flash_write_bytes = 24;
    command("UNIT g", "ERR,FLASH\r\n", now);
    assert(saved_config.unit == 1);
    assert(scale_config_load(&recovered) && recovered.unit == 1);
    flash_write_bytes = 128;
    command("UNIT g", "OK,UNIT\r\n", now);
    command("BUZZER ACTIVE", "OK,BUZZER\r\n", now);
    buzzer_start(3, now);
    for (unsigned i = 0; i < 3; ++i) {
        buzzer_poll(now += BUZZER_ON_MS);
        assert(!buzzer_on);
        if (i < 2) { buzzer_poll(now += BUZZER_GAP_MS); assert(buzzer_on); }
    }
    assert(buzzer_remaining == 0 && !(host_gpio_a.ODR & BUZZER_PIN));
    command("BUZZER OFF", "OK,BUZZER\r\n", now);
    /* 实体键短按释放时去皮，长按在空秤时归零，释放不会重复去皮。 */
    command("UNTARE", "OK,UNTARE\r\n", now);
    settle(-20000, &now);
    host_gpio_b.IDR = KEY_PIN;
    key_poll(now);
    host_gpio_b.IDR = 0;
    key_poll(++now);
    key_poll(now += KEY_DEBOUNCE_MS);
    assert(tare_counts == 0);
    host_gpio_b.IDR = KEY_PIN;
    key_poll(now += 100U);
    key_poll(now += KEY_DEBOUNCE_MS);
    assert(tare_counts == -120000);
    settle(100000, &now);
    host_gpio_b.IDR = 0;
    key_poll(++now);
    key_poll(now += KEY_DEBOUNCE_MS);
    for (unsigned i = 0; i < 20; ++i) {
        now += 100U;
        sample_add(100000, now);
        key_poll(now);
    }
    assert(tare_counts == 0 && zero_counts == 100000);
    host_gpio_b.IDR = KEY_PIN;
    key_poll(++now);
    key_poll(now += KEY_DEBOUNCE_MS);
    assert(tare_counts == 0);

    /* 空秤持续稳定且 120 秒无操作才待机，真实 0.5g 负载阻止待机和跟踪。 */
    command("UNTARE", "OK,UNTARE\r\n", now);
    settle(99500, &now);
    int32_t zero_before = zero_counts;
    for (unsigned i = 0; i < 1300; ++i) {
        now += 100U;
        sample_add(99500, now);
        power_poll(now);
    }
    assert(!sleeping && zero_counts == zero_before);
    settle(100000, &now);
    idle_since_ms = now;
    power_poll(now);
    for (unsigned i = 0; i < 1199; ++i) {
        now += 100U;
        sample_add(100000, now);
        power_poll(now);
    }
    assert(!sleeping);
    now += 100U;
    sample_add(100000, now);
    power_poll(now);
    assert(sleeping && (host_gpio_a.ODR & HX711_CLOCK_PIN));
    assert(strstr(weight_report(now), ",,0,,-1,SLEEP\r\n") != NULL);
    assert(strstr(uart_request("STATUS\r\n", ++now), "S,READY=0") != NULL);
    assert(!sleeping && !hx711_started && !(host_gpio_a.ODR & HX711_CLOCK_PIN));
    assert(strcmp(uart_request("\r\n", now), "") == 0);
    assert(strcmp(uart_request("TARE,xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n", now),
                  "ERR,COMMAND\r\n") == 0);
    assert(strcmp(uart_request("ZERO\x01\n", now), "ERR,COMMAND\r\n") == 0);
    USART1->SR = USART_SR_ORE;
    USART1->DR = 'Z';
    uart_poll(now);
    assert(strcmp(uart_request("ERO\n", now), "ERR,COMMAND\r\n") == 0);
    assert(strcmp(uart_request("RATE 100\n", now), "OK,RATE\r\n") == 0);
    assert(strstr(uart_request("HELP\n", now), "TARE[,seq]") != NULL);
    /* 从最大序号回绕到 0 仍算新请求，相同序号继续只返回原应答。 */
    tare_sequence_seen = false;
    settle(100000, &now);
    command("TARE,4294967295", "ACK,TARE,4294967295\r\n", now);
    settle(100000, &now);
    command("TARE,0", "ACK,TARE,0\r\n", now);
    command("TARE,0", "ACK,TARE,0\r\n", now);
    command("TARE,4294967296", "ERR,TARE,?,BAD_SEQ\r\n", now);

    /* 超时后停止正常帧，恢复时先重建窗口。 */
    settle(100000, &now);
    host_gpio_a.IDR = HX711_DATA_PIN;
    sample_poll(now + HX711_TIMEOUT_MS);
    weight_update(now + HX711_TIMEOUT_MS);
    assert(weight_state.status == WEIGHT_SENSOR_ERROR && !stable);
    now += HX711_TIMEOUT_MS;
    settle(100000, &now);
    assert(weight_state.status == WEIGHT_OK);
    adc_saturated = true;
    assert(strstr(weight_report(now), ",,0,,-1,ERR\r\n") != NULL);
    sample_reset();
    now = UINT32_MAX - 1000U;
    settle(100000, &now);
    assert(!adc_saturated && weight_state.status == WEIGHT_OK);

    /* 10SPS 下持续阶跃重新判稳的算法延迟应不超过 1.5s。 */
    uint32_t step_at = now;
    do {
        now += 100U;
        sample_add(99000, now); /* 新增加 1g。 */
    } while (stable && now - step_at < 500U);
    assert(!stable);
    while (!stable && now - step_at < 2000U) {
        now += 100U;
        sample_add(99000, now);
    }
    assert(stable && now - step_at <= 1500U);
    buzzer_mode = BUZZER_ACTIVE;
    buzzer_remaining = 0;
    assert(strcmp(uart_request("TARE,0\n", now), "ACK,TARE,0\r\n") == 0);
    assert(buzzer_remaining == 0); /* 重传已应答的去皮序号不再次发声。 */
    assert(strcmp(uart_request("RATE 201\n", now), "ERR,RATE_100_TO_200_MS\r\n") == 0);
    assert(buzzer_remaining == 3);
    assert(strcmp(uart_request("RATE 100\n", now), "OK,RATE\r\n") == 0);
    assert(buzzer_remaining == 1);
    printf("PASS: 3-point calibration, 0.5g preservation, segmented rounding, gross limit/OL,\n");
    printf("      sequenced TARE dedup, UART framing, persistent config and all partial-page cutoffs,\n");
    printf("      120s standby/wake, timeout/recovery, tick wrap. Simulated settling %lu ms.\n",
           (unsigned long)(now - step_at));
    return 0;
}
