/* 直接包含固件实现，模拟 ADC、实体键时序、LCD 编码和两页 Flash 的写入中断。 */
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
#define SCALE_HOST_TEST 1
#include "../../src/lcd_ht1621.c"
#include "../../src/battery.c"
#include "../../src/ble_link.c"
#define main firmware_main
#include "../../src/scale_basic.c"
#undef main

GPIO_TypeDef host_gpio_a, host_gpio_b, host_gpio_f;
USART_TypeDef host_usart;
bool host_dbg_clock_enabled, host_iwdg_frozen;

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

static void settle(int32_t counts, uint32_t *now)
{
    for (unsigned i = 0; i < 18; ++i) { *now += 100U; sample_add(counts, *now); }
    assert(stable && samples_ready(*now));
    weight_update(*now);
}

/* 实际调用消抖轮询，不直接跳过实体键状态机。持续采样避免长按造成假超时。 */
static void key_time(bool pressed, unsigned milliseconds, int32_t counts, uint32_t *now)
{
    host_gpio_b.IDR = pressed ? 0U : KEY_PIN;
    for (unsigned i = 0; i < milliseconds; ++i) {
        ++*now;
        if (*now % 100U == 0U) { sample_add(counts, *now); }
        key_poll(*now);
    }
}

static void open_item(UiMenu item, uint32_t now)
{
    ui.page = UI_MENU; ui.menu = item; ui_event(KEY_HOLD, now);
}

static void local_calibration(uint32_t *now, int direction, unsigned points)
{
    open_item(MENU_CAL, *now);
    assert(ui.page == UI_CAL_COUNT);
    ui.cal_count = points;
    ui_event(KEY_HOLD, *now);
    assert(ui.page == UI_CAL_EMPTY);
    settle(100000, now);
    unsigned before = flash_program_count;
    ui_event(KEY_HOLD, *now);
    assert(ui.page == UI_EDIT && ui.edit_kind == EDIT_MASS);
    static const uint32_t grams[] = {500,2500,5000};
    for (unsigned i = 0; i < points; ++i) {
        ui.value = grams[i];
        ui_event(KEY_HOLD, *now);
        assert(ui.page == UI_CAL_LOAD);
        settle(100000 + direction * (int32_t)(grams[i] * 1000U), now);
        ui_event(KEY_HOLD, *now);
        if (i + 1U != points) { assert(flash_program_count == before && ui.page == UI_EDIT); }
    }
    assert(ui.page == UI_WEIGH && saved_config.point_count == points);
    assert(saved_config.spans[points - 1U] == direction * (int32_t)(grams[points - 1U] * 1000U));
}

#include "completion.inc"

int main(void)
{
    watchdog_start();
    assert(host_dbg_clock_enabled && host_iwdg_frozen && watchdog_ready);
    watchdog_ready = false;
    storage_checks();
    /* 用旧布局构造真实 V1 记录，验证刷新周期字段升级不会丢失旧标定。 */
    ScaleConfig legacy;
    scale_config_defaults(&legacy);
    legacy.zero_counts = 12345; legacy.spans[0] = -500000; legacy.grams[0] = 500; legacy.point_count = 1;
    ConfigRecord v1 = {.magic=CONFIG_MAGIC,.version=1,.generation=7,.firmware_version=0x10002,.config=legacy};
    const size_t old_length = offsetof(ConfigRecord, config) + offsetof(ScaleConfig, display_interval_ms);
    uint32_t crc = config_crc(&v1, old_length);
    memcpy(host_flash, &v1, old_length); memcpy((uint8_t *)host_flash + old_length, &crc, 4);
    ScaleConfig recovered;
    assert(scale_config_load(&recovered) && recovered.zero_counts == 12345);
    assert(recovered.display_interval_ms == 200 && recovered.point_count == 1);
    recovered.display_interval_ms = 100;
    assert(scale_config_save(&recovered) && scale_config_load(&recovered));
    assert(recovered.point_count == 1 && recovered.display_interval_ms == 100);
    memset(host_flash, 0xff, sizeof(host_flash));

    scale_config_load(&saved_config);
    uint32_t now = 0;
    lcd_init();
    assert((host_gpio_f.ODR & GPIO_PIN_1) && (host_gpio_a.ODR & GPIO_PIN_4));
    ui_event(KEY_TAP, now);
    assert(strcmp(ui.message, "noCAL") == 0);
    /* 本机三点标定，负方向传感器；直到最后一步才提交一次。 */
    local_calibration(&now, -1, 3);
    assert(scale_config_load(&recovered) && recovered.point_count == 3);
    assert(gross_milligrams(-156400) == 256400);
    assert(gross_milligrams(100500) == -500);

    /* 中途退出新标定：原有 Flash、运行时零点与三点曲线完整保留。 */
    unsigned writes = flash_program_count;
    open_item(MENU_CAL, now); ui_event(KEY_HOLD, now);
    settle(200000, &now); ui_event(KEY_HOLD, now);
    assert(ui.page == UI_EDIT && ui.calibration.zero_counts == 200000);
    ui_event(KEY_MENU, now);
    assert(ui.page == UI_MENU && flash_program_count == writes);
    assert(zero_counts == 100000 && saved_config.point_count == 3);

    /* 不稳定时不能取得标定基线，也不能执行去皮。 */
    open_item(MENU_CAL, now); ui_event(KEY_HOLD, now);
    sample_reset(); ui_event(KEY_HOLD, now);
    assert(ui.page == UI_CAL_EMPTY && flash_program_count == writes);
    ui.page = UI_WEIGH;
    assert(strcmp(set_tare(now), "ERR,WAIT_SAMPLES\r\n") == 0);

    /* 非线性曲线先分别换算毛重与皮重，再求净重。 */
    saved_config.spans[1] = -2400000; saved_config.spans[2] = -4900000;
    tare_counts = -500000;
    settle(-1350000, &now);
    assert(weight_state.net_mg == 1000000);
    saved_config = recovered; tare_counts = 0;
    settle(99500, &now);
    assert(weight_state.net_mg == 500 && weight_state.display_mg == 500);
    assert(strcmp(set_zero(now), "ERR,NOT_EMPTY\r\n") == 0);
    assert(zero_counts == 100000);
    settle(100000, &now);
    sample_add(-8000000, ++now);
    assert(stable && filtered_counts() == 100000);
    sample_add(100000, ++now); assert(filtered_counts() == 100000);
    weight_state.division_mg = 100;
    assert(select_division(200000) == 100 && select_division(200600) == 500);
    weight_state.division_mg = 500;
    assert(select_division(199600) == 500 && select_division(199400) == 100);
    assert(select_division(1001100) == 1000);
    weight_state.division_mg = 1000;
    assert(select_division(999500) == 1000 && select_division(998900) == 500);

    /* 实体双击仅切单位，不会先去皮；第二次按住超过双击窗口仍算同一双击。 */
    ui.page = UI_WEIGH;
    settle(-20000, &now);
    key_time(false, 50, -20000, &now);
    key_time(true, 100, -20000, &now); key_time(false, 100, -20000, &now);
    assert(tare_counts == 0);
    key_time(true, 500, -20000, &now); key_time(false, 400, -20000, &now);
    assert(saved_config.unit == 1 && tare_counts == 0);
    key_time(true, 100, -20000, &now); key_time(false, 400, -20000, &now);
    assert(tare_counts == -120000);
    /* 持续三秒不提前归零，释放后进入菜单。 */
    key_time(true, 3200, 100000, &now);
    assert(ui.page == UI_WEIGH && tare_counts == -120000);
    key_time(false, 40, 100000, &now);
    assert(ui.page == UI_MENU && tare_counts == -120000);
    ui_event(KEY_DOUBLE, now);
    key_time(true, 1700, 100000, &now); key_time(false, 400, 100000, &now);
    assert(tare_counts == 0 && zero_counts == 100000 && ui.page == UI_WEIGH);

    /* 毛重限制不能被去皮绕过，移走容器显示负数。 */
    settle(-2900000, &now); ui_event(KEY_TAP, now);
    settle(-4901000, &now); assert(weight_state.status == WEIGHT_RANGE);
    assert(strcmp(set_tare(now), "ERR,OVERLOAD\r\n") == 0);
    settle(-5150000, &now); assert(weight_state.status == WEIGHT_OVERLOAD);
    settle(100000, &now); assert(weight_state.display_mg == -3000000);
    open_item(MENU_UNTARE, now); assert(tare_counts == 0 && ui.page == UI_WEIGH);

    /* 所有设置由本机菜单提交：非法值、非法日期和半页写入均不破坏旧配置。 */
    open_item(MENU_OFF, now); ui.value=86401; ui_event(KEY_HOLD,now);
    assert(ui.page==UI_EDIT && saved_config.auto_off_seconds==120);
    ui.value=120; ui_event(KEY_HOLD,now); assert(ui.page==UI_MENU);
    open_item(MENU_RATE,now); ui.value=100; ui_event(KEY_HOLD,now);
    assert(saved_config.display_interval_ms==100 && display_interval_ms==100);
    open_item(MENU_STABLE,now); ui.value=1000000; ui_event(KEY_HOLD,now);
    assert(saved_config.stable_counts==1000000 && ui.page==UI_MENU);
    open_item(MENU_STABLE,now); ui.value=64; ui_event(KEY_HOLD,now);
    open_item(MENU_ID,now); ui.value=123; ui_event(KEY_HOLD,now);
    assert(strcmp(saved_config.device_id,"SCALE-000123")==0);
    open_item(MENU_DATE,now); ui.value=2024; ui_event(KEY_HOLD,now);
    assert(ui.edit_kind==EDIT_MONTH); ui.value=2; ui_event(KEY_HOLD,now);
    ui.value=29; ui_event(KEY_HOLD,now); assert(saved_config.calibration_date==20240229);
    assert(!ui_date_valid(2026,2,29) && ui_date_valid(2000,2,29) && !ui_date_valid(2100,2,29));
    open_item(MENU_DATE,now); ui.value=2026; ui_event(KEY_HOLD,now); ui.value=2; ui_event(KEY_HOLD,now);
    ui.value=29; ui_event(KEY_HOLD,now); assert(ui.page==UI_EDIT && saved_config.calibration_date==20240229);
    ui_event(KEY_MENU,now);
    open_item(MENU_UNIT,now); unsigned unit=saved_config.unit;
    ui_event(KEY_TAP,now); flash_write_bytes=24; ui_event(KEY_HOLD,now);
    assert(saved_config.unit==unit && ui.page==UI_CHOICE);
    flash_write_bytes=128; ui_event(KEY_HOLD,now); assert(saved_config.unit!=(uint32_t)unit);
    assert(scale_config_load(&recovered) && memcmp(&recovered,&saved_config,sizeof(recovered))==0);

    /* 数字编辑：短按当前位加一、双击移位、长按取消，普通返回不会保存草稿。 */
    open_item(MENU_ID,now); ui.value=99;
    ui_event(KEY_TAP,now); assert(ui.value==90);
    ui_event(KEY_DOUBLE,now); assert(ui.cursor==1);
    ui_event(KEY_TAP,now); assert(ui.value==0);
    ui_event(KEY_MENU,now); assert(strcmp(saved_config.device_id,"SCALE-000123")==0);
    open_item(MENU_BUZZER,now); ui.choice=BUZZER_ACTIVE; ui_event(KEY_HOLD,now);
    buzzer_start(3,now);
    for (unsigned i=0;i<3;++i) {
        buzzer_poll(now+=BUZZER_ON_MS); assert(!buzzer_on);
        if(i<2){buzzer_poll(now+=BUZZER_GAP_MS); assert(buzzer_on);}
    }
    assert(buzzer_remaining==0 && !(host_gpio_a.ODR & BUZZER_PIN));
    open_item(MENU_BUZZER,now); ui.choice=BUZZER_OFF; ui_event(KEY_HOLD,now);

    /* 0.5g 真实小负载连续130秒不被零点跟踪吞掉、不触发待机。 */
    ui.page=UI_WEIGH; settle(99500,&now); int32_t old_zero=zero_counts;
    for(unsigned i=0;i<1300;++i){now+=100;sample_add(99500,now);power_poll(now);}
    assert(!sleeping && zero_counts==old_zero);
    settle(100000,&now); idle_since_ms=now; power_poll(now);
    for(unsigned i=0;i<1199;++i){now+=100;sample_add(100000,now);power_poll(now);}
    assert(!sleeping);
    now+=100;sample_add(100000,now);power_poll(now);
    assert(sleeping && !enabled && (host_gpio_a.ODR & HX711_CLOCK_PIN));
    key_time(true,100,100000,&now); key_time(false,400,100000,&now);
    assert(!sleeping && enabled && tare_counts==0 && ui.page==UI_WEIGH);

    lcd_set_missing_mask(0);
    /* LCD: g/kg 小数位置、负号与溢出显示，以及菜单标题、空白和自检。 */
    lcd_weight(500,true,true,false); assert((frame[10+1] & 8U)!=0); /* 0.0005kg */
    lcd_weight(500,false,true,true); assert((frame[18+1] & 8U)!=0); /* 0.5g */
    lcd_number("g",-1234,1); assert(frame[10]==2U && frame[11]==0U); /* 第二位负号 */
    lcd_number("g",1000000,0); for(unsigned i=0;i<6;++i){static const unsigned a[]={8,10,12,16,18,20};assert(frame[a[i]]==2U && frame[a[i]+1]==0U);}
    lcd_text("", "ZERO",0); assert(frame[8]!=0);
    lcd_test_all(); for(unsigned i=0;i<32;++i){assert(frame[i]==15);}
    lcd_clear(); for(unsigned i=0;i<32;++i){assert(frame[i]==0);}
    /* 逐位回归：上方第四位的4必须使用SEG6/7，SEG22不能被写入。
     * 读回测试用GPIO输入固定低/高模拟失败与匹配，防止把发送缓存误当成器件RAM。
     */
    lcd_text("   4", "", 0); assert(frame[6]==12 && frame[7]==6 && frame[22]==0);
    host_gpio_a.IDR = 0; assert(!lcd_verify_ram() && lcd_ram_checked==1);
    assert(lcd_ram_mismatch==((1UL<<6)|(1UL<<7)));
    lcd_clear(); assert(lcd_verify_ram() && lcd_ram_mismatch==0);
    host_gpio_a.IDR=GPIO_PIN_7; lcd_test_all(); assert(lcd_verify_ram());
    for(unsigned i=0;i<32;++i){assert(lcd_ram_readback[i]==15);}
    /* 菜单按产品编号显示：第7项必须在上方为07，数值区留空。 */
    ui.startup_ms=0;ui.message=NULL;ui.page=UI_MENU;ui.menu=MENU_CAL;ui.rendered_ms=0;
    ui_render(4000);
    assert(frame[4]==11 && frame[5]==14 && frame[6]==0 && frame[7]==14);
    for(unsigned i=8;i<32;++i){assert(frame[i]==0);}

    /* 超时、恢复和 tick 回绕；持续阶跃恢复稳定的模拟延迟。 */
    settle(100000,&now); host_gpio_a.IDR=HX711_DATA_PIN;
    sample_poll(now+HX711_TIMEOUT_MS); now+=HX711_TIMEOUT_MS; weight_update(now);
    assert(weight_state.status==WEIGHT_SENSOR_ERROR && !stable);
    settle(100000,&now); assert(weight_state.status==WEIGHT_OK);
    sample_reset();now=UINT32_MAX-1000U;settle(100000,&now);
    uint32_t step_at=now;
    do { now+=100;sample_add(99000,now); } while(stable && now-step_at<500U);
    assert(!stable);
    while(!stable && now-step_at<2000U){now+=100;sample_add(99000,now);}
    assert(stable && now-step_at<=1500U);
    /* 正方向也可完成一/二/三点标定。 */
    local_calibration(&now,1,1); local_calibration(&now,1,2); local_calibration(&now,1,3);
    /* 手头最大砝码为20g：完整走默认单点向导，不直接改标定数组。
     * 20g只是标定点，量程仍为5000g；小质量输入不显示前导零。
     */
    open_item(MENU_CAL,now); assert(ui.cal_count==1);
    ui_event(KEY_HOLD,now); settle(100000,&now); ui_event(KEY_HOLD,now);
    assert(ui.page==UI_EDIT && ui.edit_kind==EDIT_MASS && ui.value==20);
    ui.startup_ms=now-4000U; ui.message=NULL; ui.rendered_ms=now-200U;
    ui_render(now); assert(frame[12]==0 && frame[13]==0); /* 第三位为空 */
    ui_event(KEY_HOLD,now); settle(120000,&now); ui_event(KEY_HOLD,now);
    assert(ui.page==UI_WEIGH && saved_config.point_count==1 && saved_config.grams[0]==20);
    assert(saved_config.spans[0]==20000 && reference_grams==20 && MAX_GROSS_GRAMS==5000);
    assert(scale_config_load(&recovered) && recovered.grams[0]==20);
    settle(100000,&now); assert(weight_state.display_mg==0);
    settle(120000,&now); assert(weight_state.display_mg==20000);
    settle(5101000,&now); assert(weight_state.status==WEIGHT_RANGE); /* 5001g仍超范围 */
    local_calibration(&now,1,3);
    open_item(MENU_CLEAR,now); assert(saved_config.point_count==3);
    ui_event(KEY_DOUBLE,now); assert(saved_config.point_count==3);
    open_item(MENU_CLEAR,now);ui_event(KEY_HOLD,now);
    assert(saved_config.point_count==0 && reference_grams==0 && ui.page==UI_WEIGH);
    completion_checks();
    printf("PASS: physical key debounce/double/long/wake; local menus and atomic 1/2/3-point calibration;\n");
    printf("      LCD digits/decimal/sign; persistent settings and V1 migration; all partial-page cutoffs;\n");
    printf("      small-load preservation, gross overload, negative net, standby, recovery and tick wrap.\n");
    return 0;
}
