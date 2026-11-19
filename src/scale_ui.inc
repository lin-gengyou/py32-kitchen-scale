/*
 * 本机操作界面。与称重核心在同一编译单元内，直接复用标定、去皮和存储状态。
 * SW4：短按切换，双击确认，长按进出菜单。数字编辑时约2秒长按换位。
 */
typedef enum { UI_WEIGH, UI_MENU, UI_CHOICE, UI_EDIT, UI_CAL_COUNT,
    UI_CAL_EMPTY, UI_CAL_LOAD, UI_CONFIRM_CLEAR, UI_INFO=9, UI_TEST, UI_BATTERY, UI_CAL_VERIFY } UiPage;
typedef enum { MENU_GROSS, MENU_UNIT, MENU_BUZZER, MENU_OFF, MENU_CAL, MENU_INFO,
    MENU_DATE, MENU_CLEAR, MENU_TEST, MENU_SLEEP, MENU_BATTERY, MENU_DISPLAY,
    MENU_BLUETOOTH, MENU_RESET, MENU_COUNT } UiMenu;
typedef enum { KEY_TAP, KEY_DOUBLE, KEY_HOLD, KEY_MENU } KeyEvent;
enum { EDIT_MASS, EDIT_YEAR, EDIT_MONTH, EDIT_DAY };
static const uint16_t standby_seconds[] = {0, 30, 60, 120, 300, 600};
static unsigned standby_choice(uint32_t seconds)
{
    for (unsigned i = 0; i < sizeof(standby_seconds)/sizeof(standby_seconds[0]); ++i) {
        if (standby_seconds[i] == seconds) { return i; }
    }
    return 3U;
}

static struct {
    UiPage page;
    UiMenu menu;
    unsigned choice, edit_kind, cursor, info_page, cal_index, cal_count;
    uint32_t value, minimum, maximum, edit_year, edit_month;
    uint32_t startup_ms, rendered_ms, message_ms;
    const char *message;
    ScaleConfig calibration;           /* 标定过程先写草稿，最后一次性提交。 */
    uint32_t cal_zero_ms, cal_load_ms[3];
} ui;
static const uint32_t display_interval_ms = 200;

static void ui_message(const char *text, bool ok, uint32_t now)
{
    ui.message = text;
    ui.message_ms = now;
    buzzer_start(ok ? 1U : 3U, now);
}

static void ui_result(MeasurementResult result, uint32_t now)
{
    static const char *const text[] = {"donE", "noCAL", "no Ad", "UnSt", "OL", "bAtt", "LoAd", "FLASH"};
    ui_message(text[result], result == MEAS_OK, now);
}

static void ui_tare_result(MeasurementResult result, uint32_t now)
{
    /* 去皮完成后立即恢复重量页，成功提示音保持。 */
    if (result == MEAS_OK) {
        ui.message = NULL;
        buzzer_start(1U, now);
        return;
    }
    ui_result(result, now);
}

static void ui_save(const ScaleConfig *candidate, uint32_t now)
{
    if (save_settings(candidate)) { ui.page = UI_MENU; ui_message("SAvEd", true, now); }
    else { ui_message("FLASH", false, now); }
}

static uint32_t power10(unsigned n)
{
    uint32_t p = 1;
    while (n-- != 0U) { p *= 10U; }
    return p;
}

static void ui_edit(unsigned kind, uint32_t value, uint32_t low, uint32_t high)
{
    ui.page = UI_EDIT; ui.edit_kind = kind; ui.value = value;
    ui.minimum = low; ui.maximum = high; ui.cursor = 0;
}

static uint32_t ui_device_number(void)
{
    uint32_t value = 0;
    for (const char *p = saved_config.device_id; *p != '\0'; ++p) {
        if (*p >= '0' && *p <= '9') { value = (value * 10U + (unsigned)(*p - '0')) % 1000000U; }
    }
    return value;
}

static bool ui_date_valid(uint32_t year, uint32_t month, uint32_t day)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
    return year >= 2000U && year <= 9999U && month >= 1U && month <= 12U && day >= 1U &&
        day <= days[month - 1U] + (unsigned)(month == 2U && leap);
}

static void ui_confirm_edit(uint32_t now)
{
    if (ui.value < ui.minimum || ui.value > ui.maximum) { ui_message("rAnGE", false, now); return; }
    ScaleConfig candidate = saved_config;
    switch (ui.edit_kind) {
    case EDIT_MASS:
        if (ui.cal_index != 0U && ui.value <= ui.calibration.grams[ui.cal_index - 1U]) {
            ui_message("OrdEr", false, now); return;
        }
        ui.calibration.grams[ui.cal_index] = ui.value;
        ui.page = UI_CAL_LOAD; return;
    case EDIT_YEAR:
        ui.edit_year = ui.value;
        ui_edit(EDIT_MONTH, saved_config.calibration_date / 100U % 100U, 1, 12);
        if (ui.value == 0U) { ui.value = 1; } return;
    case EDIT_MONTH:
        ui.edit_month = ui.value;
        ui_edit(EDIT_DAY, saved_config.calibration_date % 100U, 1, 31);
        if (ui.value == 0U) { ui.value = 1; } return;
    case EDIT_DAY:
        if (!ui_date_valid(ui.edit_year, ui.edit_month, ui.value)) { ui_message("dAtE", false, now); return; }
        candidate.calibration_date = ui.edit_year * 10000U + ui.edit_month * 100U + ui.value;
        break;
    default: return;
    }
    ui_save(&candidate, now);
}

static void ui_begin_item(uint32_t now)
{
    switch (ui.menu) {
    case MENU_GROSS:
        show_gross = !show_gross;
        display_locked = false;
        weight_state.status = WEIGHT_WAIT;
        weight_update(now);
        ui.page = UI_WEIGH;
        break;
    case MENU_UNIT: ui.page = UI_CHOICE; ui.choice = saved_config.unit; break;
    case MENU_BUZZER: ui.page = UI_CHOICE; ui.choice = saved_config.buzzer_mode; break;
    case MENU_OFF:
        ui.page = UI_CHOICE; ui.choice = standby_choice(saved_config.auto_off_seconds);
        break;
    case MENU_CAL: ui.page = UI_CAL_COUNT; ui.cal_count = 1; break;
    case MENU_INFO: ui.page = UI_INFO; ui.info_page = 0; break;
    case MENU_DATE: ui_edit(EDIT_YEAR, saved_config.calibration_date / 10000U, 2000, 9999);
        if (ui.value == 0U) { ui.value = 2026; } break;
    case MENU_CLEAR: ui.page = UI_CONFIRM_CLEAR; break;
    case MENU_TEST: ui.page = UI_TEST; ui.info_page = 0; break;
    case MENU_SLEEP: ui.page = UI_WEIGH; enter_standby(); break;
    case MENU_BATTERY: ui.page = UI_BATTERY; ui.info_page = 0; break;
    case MENU_DISPLAY: ui.page = UI_CHOICE; ui.choice = saved_config.lcd_missing_mask != 0U; break;
    case MENU_BLUETOOTH: ui.page = UI_CHOICE; ui.choice = saved_config.bluetooth_enabled; break;
    case MENU_RESET: ui.page = UI_CONFIRM_CLEAR; break;
    default: break;
    }
}

static void ui_cal_next_mass(void)
{
    static const uint16_t defaults[] = {500,2500,5000};
    ui_edit(EDIT_MASS, ui.cal_count == 1U ? 20U : defaults[ui.cal_index], 1, 5000);
}

static void ui_cal_apply(int32_t counts, uint32_t now)
{
    if (ui.page == UI_CAL_EMPTY) {
        ui.calibration = saved_config;
        ui.calibration.zero_counts = counts;
        ui.cal_zero_ms = now;
        ui.calibration.point_count = 0;
        memset(ui.calibration.spans, 0, sizeof(ui.calibration.spans));
        memset(ui.calibration.grams, 0, sizeof(ui.calibration.grams));
        ui.cal_index = 0;
        ui_cal_next_mass(); return;
    }
    if (ui.page!=UI_CAL_VERIFY) {
        ui.calibration.spans[ui.cal_index] = counts - ui.calibration.zero_counts;
        ui.cal_load_ms[ui.cal_index] = now;
        ui.calibration.point_count = ui.cal_index + 1U;
        if (!scale_config_valid(&ui.calibration)) { ui_message("SPAn", false, now); return; }
        if (ui.calibration.point_count < ui.cal_count) {
            ++ui.cal_index; ui_cal_next_mass(); return;
        }
        ui.page=UI_CAL_VERIFY; return; /* 卸载回零确认后才保存，防止错误零点被当成成功。 */
    }
    int32_t shift=counts-ui.calibration.zero_counts;
    int64_t span=ui.calibration.spans[0];if(span<0){span=-span;}
    int64_t distance=shift<0?-(int64_t)shift:shift;
    if(distance*ui.calibration.grams[0]*5>span){ui_message("drFt",false,now);return;}
    /* 调整只作用于提交副本，Flash失败重试仍与本轮最初空秤比较。 */
    ScaleConfig candidate=ui.calibration;
    uint32_t elapsed = now - ui.cal_zero_ms;
    if (shift != 0 && elapsed != 0U) {
        /* 以两次空秤之间的时间比例，估算各加载点当时的空载偏移。 */
        for (unsigned i = 0; i < candidate.point_count; ++i) {
            uint32_t load_elapsed = ui.cal_load_ms[i] - ui.cal_zero_ms;
            candidate.spans[i] -= (int32_t)((int64_t)shift * load_elapsed / elapsed);
        }
    }
    candidate.zero_counts=counts;
    if(!scale_config_valid(&candidate)){ui_message("SPAn",false,now);return;}
    if (!save_settings(&candidate)) { ui_message("FLASH", false, now); return; }
    startup_zero_pending = false;
    zero_counts = saved_config.zero_counts;
    reference_grams = saved_config.grams[saved_config.point_count - 1U];
    zero_set = true; empty_reference_known = true; empty_candidate = false; tare_counts = 0;
    ui.page = UI_WEIGH;
    filter_seed(counts,now);
    show_gross = false;
    zero_locked = true;
    ui_message("SAvEd", true, now);
}

static void ui_event(KeyEvent event, uint32_t now)
{
    idle_since_ms = now;
    if (capture.kind!=MEASURE_NONE) {
        if (event != KEY_MENU && event != KEY_HOLD) { return; }
        capture.kind=MEASURE_NONE;
        if(ui.page==UI_WEIGH){ui_message("CAnCL",true,now);return;}
    }
    buzzer_start(1, now);
    ui.message = NULL;
    if (ui.page == UI_WEIGH) {
        if (event == KEY_MENU || event == KEY_HOLD) { ui.page = UI_MENU; ui.menu = MENU_GROSS; }
        else if (event == KEY_DOUBLE) { measurement_request(MEASURE_ZERO,now); }
        else if (event == KEY_TAP) { measurement_request(MEASURE_TARE,now); }
        return;
    }
    if (event == KEY_MENU || (event == KEY_HOLD && ui.page != UI_EDIT)) {
        ui.page = ui.page == UI_MENU ? UI_WEIGH : UI_MENU;
        return;
    }
    if (ui.page == UI_EDIT) {
        if (event == KEY_DOUBLE) { ui_confirm_edit(now); }
        else if (event == KEY_HOLD) {
            unsigned digits = 1; while (power10(digits) <= ui.maximum) { ++digits; }
            ui.cursor = (ui.cursor + 1U) % digits;
        } else {
            uint32_t place = power10(ui.cursor);
            unsigned old = ui.value / place % 10U;
            ui.value = ui.value - old * place + ((old + 1U) % 10U) * place;
        }
        return;
    }
    if (ui.page == UI_MENU) {
        if (event == KEY_TAP) { ui.menu = (UiMenu)((ui.menu + 1U) % MENU_COUNT); }
        else { ui_begin_item(now); }
    } else if (ui.page == UI_CHOICE) {
        if (event == KEY_TAP) { ui.choice = (ui.choice + 1U) % (ui.menu == MENU_OFF ? 6U : ui.menu == MENU_BUZZER ? 3U : 2U); }
        else {
            ScaleConfig candidate = saved_config;
            if (ui.menu == MENU_UNIT) { candidate.unit = ui.choice; }
            else if (ui.menu == MENU_BUZZER) { candidate.buzzer_mode = ui.choice; }
            else if (ui.menu == MENU_OFF) { candidate.auto_off_seconds = standby_seconds[ui.choice]; }
            else if (ui.menu == MENU_DISPLAY) { candidate.lcd_missing_mask = ui.choice ? 4U : 0U; }
            else { candidate.bluetooth_enabled = ui.choice; }
            ui_save(&candidate, now);
        }
    } else if (ui.page == UI_CAL_COUNT) {
        if (event == KEY_TAP) { ui.cal_count = ui.cal_count % 3U + 1U; }
        else { ui.page = UI_CAL_EMPTY; }
    } else if (ui.page == UI_CAL_EMPTY || ui.page == UI_CAL_LOAD || ui.page==UI_CAL_VERIFY) {
        if (event == KEY_DOUBLE) { measurement_request(MEASURE_CAL,now); }
    } else if (ui.page == UI_CONFIRM_CLEAR && event == KEY_DOUBLE) {
        ScaleConfig candidate = saved_config;
        if (ui.menu == MENU_RESET) { scale_config_defaults(&candidate); }
        candidate.point_count = 0; candidate.zero_counts = 0;
        memset(candidate.spans, 0, sizeof(candidate.spans)); memset(candidate.grams, 0, sizeof(candidate.grams));
        if (save_settings(&candidate)) {
            zero_counts = 0; reference_grams = 0; tare_counts = 0; zero_set = false; empty_reference_known = false;
            show_gross = false; zero_locked = false;
            ui.page = UI_WEIGH; ui_message("CLr", true, now);
        } else { ui_message("FLASH", false, now); }
    } else if (ui.page == UI_INFO || ui.page == UI_TEST || ui.page == UI_BATTERY) {
        if (event == KEY_DOUBLE) { ui.page = UI_MENU; }
        else { ++ui.info_page; }
    }
}

/* 30ms消抖，350ms双击窗口。短按延后确认，双击不会先执行一次去皮。 */
static void key_poll(uint32_t now)
{
    static bool physical, debounced, pending, wake_press, second_press;
    static uint32_t changed, down, released;
    bool pressed = (KEY_PORT->IDR & KEY_PIN) == 0U;
    if (pressed) { idle_since_ms = now; }
    if (pressed != physical) { physical = pressed; changed = now; }
    if (pressed != debounced && (uint32_t)(now - changed) >= KEY_DEBOUNCE_MS) {
        debounced = pressed;
        if (pressed) {
            second_press = pending && (uint32_t)(now - released) <= 350U;
            if (pending && !second_press) { pending = false; ui_event(KEY_TAP, now); }
            down = now;
            wake_press = sleeping;
            if (sleeping) { wake_scale(now); pending = false; }
        } else if (!wake_press) {
            uint32_t duration = now - down;
            if (duration >= 1500U) {
                pending = false; ui_event(duration >= 3000U ? KEY_MENU : KEY_HOLD, now);
            } else if (pending && second_press) {
                pending = false; ui_event(KEY_DOUBLE, now);
            } else { pending = true; released = now; }
        }
    }
    if (pending && !debounced && (uint32_t)(now - released) > 350U) {
        pending = false; ui_event(KEY_TAP, now);
    }
}

static void ui_render(uint32_t now)
{
    if (sleeping) { return; }
    if ((uint32_t)(now - ui.rendered_ms) < display_interval_ms) { return; }
    ui.rendered_ms = now;
    if ((uint32_t)(now - ui.startup_ms) < 2500U) { lcd_test_all(); }
    else if ((uint32_t)(now - ui.startup_ms) < 3500U) { lcd_text("vEr", SCALE_VERSION_DISPLAY, 0x03); }
    else if(capture.kind!=MEASURE_NONE){lcd_text("buSy","WAIT",0);}
    else if (ui.message != NULL && (uint32_t)(now - ui.message_ms) < 1200U) { lcd_text("", ui.message, 0); }
    else if (ui.page == UI_WEIGH) {
        switch (weight_state.status) {
        case WEIGHT_OK:
            lcd_weight(weight_state.display_mg, saved_config.unit != 0U, weight_state.stable,
                show_gross ? LCD_GROSS : tare_counts != 0 ? LCD_TARE : LCD_NORMAL);
            break;
        case WEIGHT_UNCALIBRATED: lcd_text("CAL", "noCAL", 0); break;
        case WEIGHT_SENSOR_ERROR: lcd_text("Err", "SEnSr", 0); break;
        case WEIGHT_LOW_BATTERY: lcd_number("bAtt", battery_state()->millivolts, 3); break;
        case WEIGHT_POWER_ERROR: lcd_text("bAtt", "Err", 0); break;
        case WEIGHT_RANGE: lcd_text("Err", "rAnGE", 0); break;
        case WEIGHT_OVERLOAD: lcd_text("Err", "  OL", 0); break;
        default: lcd_text("", "------", 0); break;
        }
        const BatteryState *power = battery_state();
        if (power->valid && (power->status < BATTERY_LOW || now / 500U % 2U)) { lcd_battery(power->percent); }
    } else if (ui.page == UI_MENU || ui.page == UI_CHOICE) {
        /* 七段屏用数字编号，避免将长单词拼成难辨认的字形。 */
        unsigned code = (unsigned)ui.menu + 1U;
        char title[5] = {' ', ' ', (char)('0' + code / 10U), (char)('0' + code % 10U), 0};
        if (ui.page == UI_MENU) { lcd_text(title, "", 0); }
        else if (ui.menu == MENU_UNIT) { lcd_text(title, ui.choice ? "kg" : "g", 0); }
        else if (ui.menu == MENU_OFF) { lcd_number(title, standby_seconds[ui.choice], 0); }
        else if (ui.menu == MENU_DISPLAY) { lcd_number(title, 6U - ui.choice, 0); }
        else { lcd_number(title, (int32_t)ui.choice, 0); }
    } else if (ui.page == UI_EDIT) {
        static const char *const titles[] = {"CAL","YEAr","Mon","dAY"};
        unsigned width = lcd_body_width();
        unsigned low = ui.cursor >= width ? ui.cursor - width + 1U : 0U;
        uint32_t part = ui.value / power10(low);
        char text[7] = "000000";
        text[width] = '\0';
        for (unsigned i = 0; i < width; ++i) { text[width - 1U - i] = (char)('0' + part % 10U); part /= 10U; }
        /* 砝码质量按实际位数显示；空白前导位不占据已知缺段的第三位。
         * 被选中的高位仍显示0，便于逐位修改。
         */
        unsigned selected = width - 1U - (ui.cursor - low);
        if (ui.edit_kind == EDIT_MASS) {
            for (unsigned i = 0; i < width - 1U && text[i] == '0'; ++i) {
                if (i != selected) { text[i] = ' '; }
            }
        }
        if (now / 300U % 2U == 0U) { text[selected] = ' '; }
        lcd_text(titles[ui.edit_kind], text, 0);
    } else if (ui.page == UI_CAL_COUNT) { lcd_number("CAL", (int32_t)ui.cal_count, 0); }
    else if (ui.page == UI_CAL_EMPTY || ui.page==UI_CAL_VERIFY) { lcd_text(ui.page==UI_CAL_VERIFY ? "End" : stable && samples_ready(now) ? "S" : "", "ZERO", 0); }
    else if (ui.page == UI_CAL_LOAD) {
        char title[5] = {'C',(char)('1' + ui.cal_index), 'g', stable && samples_ready(now) ? 'S' : ' ',0};
        lcd_number(title, (int32_t)ui.calibration.grams[ui.cal_index], 0);
    } else if (ui.page == UI_CONFIRM_CLEAR) { lcd_text(ui.menu == MENU_RESET ? "1400" : "0800", "SUrE", 0); }
    else if (ui.page == UI_INFO) {
        switch (ui.info_page % 7U) {
        case 0: lcd_text("vEr", SCALE_VERSION_DISPLAY, 0x03); break;
        case 1: lcd_number("IdH", (int32_t)(ui_device_number() / power10(lcd_body_width())), 0); break;
        case 2: lcd_number("IdL", (int32_t)(ui_device_number() % power10(lcd_body_width())), 0); break;
        case 3: lcd_number("YEAr", (int32_t)(saved_config.calibration_date / 10000U), 0); break;
        case 4: lcd_number("dAtE", (int32_t)(saved_config.calibration_date % 10000U), 0); break;
        case 5: lcd_number("CAP", MAX_GROSS_GRAMS, 0); break;
        default: lcd_number("CAL", (int32_t)saved_config.point_count, 0); break;
        }
    } else if (ui.page == UI_BATTERY) {
        const BatteryState *power = battery_state();
        unsigned page = ui.info_page % 4U;
        char title[5] = {'1','1','0',(char)('1' + page),0};
        int32_t value = page == 0U ? power->millivolts : page == 1U ? power->supply_mv :
            page == 2U ? power->percent : power->status;
        if ((page < 2U && !power->valid) || (page == 2U && value < 0)) {
            lcd_text(title, "-----", 0);
        } else { lcd_number(title, value, 0); }
    } else if (ui.page == UI_TEST) {
        if (ui.info_page % 4U == 0U) { lcd_test_all(); }
        else if (ui.info_page % 4U == 1U) { lcd_text("1234", "123456", 0x1f); }
        else if (ui.info_page % 4U == 2U) { lcd_clear(); }
        else { lcd_number("0904", (int32_t)lcd_ram_result(), 0); }
    }
    lcd_flush();
    /* 结果页保留上一幅测试图的核对结果；全亮/数字/空白页实际读回HT1621。 */
    if (ui.page == UI_TEST && ui.info_page % 4U != 3U) { (void)lcd_verify_ram(); }
}
