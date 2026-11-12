/*
 * 本机操作界面。与称重核心在同一编译单元内，直接复用标定、去皮和存储状态。
 * SW4：短按/双击/1.5 秒长按/3 秒长按。长按动作在松手时确认，不会先误触归零。
 * 菜单短按下一项，1.5 秒长按确认，双击返回；数字编辑双击移位，3 秒长按取消。
 */
typedef enum { UI_WEIGH, UI_MENU, UI_CHOICE, UI_EDIT, UI_CAL_COUNT,
    UI_CAL_EMPTY, UI_CAL_LOAD, UI_CONFIRM_CLEAR, UI_RAW, UI_INFO, UI_TEST } UiPage;
typedef enum { MENU_UNTARE, MENU_UNIT, MENU_BUZZER, MENU_OFF, MENU_STABLE,
    MENU_RATE, MENU_CAL, MENU_RAW, MENU_INFO, MENU_ID, MENU_DATE, MENU_CLEAR,
    MENU_TEST, MENU_SLEEP, MENU_EXIT, MENU_COUNT } UiMenu;
typedef enum { KEY_TAP, KEY_DOUBLE, KEY_HOLD, KEY_MENU } KeyEvent;
enum { EDIT_OFF, EDIT_STABLE, EDIT_RATE, EDIT_MASS, EDIT_ID, EDIT_YEAR, EDIT_MONTH, EDIT_DAY };

static struct {
    UiPage page;
    UiMenu menu;
    unsigned choice, edit_kind, cursor, info_page, cal_index, cal_count;
    uint32_t value, minimum, maximum, edit_year, edit_month;
    uint32_t startup_ms, rendered_ms, message_ms;
    const char *message;
    ScaleConfig calibration;           /* 标定过程先写草稿，最后一次性提交。 */
} ui;
static uint32_t display_interval_ms = 200;

static void ui_message(const char *text, bool ok, uint32_t now)
{
    ui.message = text;
    ui.message_ms = now;
    buzzer_start(ok ? 1U : 3U, now);
}

static void ui_result(const char *result, uint32_t now)
{
    bool ok = result[0] == 'O';
    const char *text = "donE";
    if (!ok) {
        text = strcmp(result, "ERR,UNSTABLE\r\n") == 0 ? "UnSt" :
            strcmp(result, "ERR,WAIT_SAMPLES\r\n") == 0 ? "no Ad" :
            strcmp(result, "ERR,NEED_CAL\r\n") == 0 ? "no CAL" :
            strcmp(result, "ERR,NOT_EMPTY\r\n") == 0 ? "LoAd" : "OL";
    }
    ui_message(text, ok, now);
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
    case EDIT_OFF: candidate.auto_off_seconds = ui.value; break;
    case EDIT_STABLE: candidate.stable_counts = ui.value; break;
    case EDIT_RATE: candidate.display_interval_ms = ui.value; break;
    case EDIT_MASS:
        if (ui.cal_index != 0U && ui.value <= ui.calibration.grams[ui.cal_index - 1U]) {
            ui_message("OrdEr", false, now); return;
        }
        ui.calibration.grams[ui.cal_index] = ui.value;
        ui.page = UI_CAL_LOAD; return;
    case EDIT_ID:
        memcpy(candidate.device_id, "SCALE-000000", 13);
        for (unsigned i = 0; i < 6; ++i) {
            candidate.device_id[11U - i] = (char)('0' + ui.value / power10(i) % 10U);
        }
        break;
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
    display_interval_ms = saved_config.display_interval_ms;
    if (ui.edit_kind == EDIT_STABLE && ui.page == UI_MENU) { sample_reset(); }
}

static void ui_begin_item(uint32_t now)
{
    switch (ui.menu) {
    case MENU_UNTARE: tare_counts = 0; weight_update(now); ui.page = UI_WEIGH; ui_message("donE", true, now); break;
    case MENU_UNIT: ui.page = UI_CHOICE; ui.choice = saved_config.unit; break;
    case MENU_BUZZER: ui.page = UI_CHOICE; ui.choice = saved_config.buzzer_mode; break;
    case MENU_OFF: ui_edit(EDIT_OFF, saved_config.auto_off_seconds, 0, 86400); break;
    case MENU_STABLE: ui_edit(EDIT_STABLE, saved_config.stable_counts, 1, 1000000); break;
    case MENU_RATE: ui_edit(EDIT_RATE, display_interval_ms, 100, 200); break;
    case MENU_CAL: ui.page = UI_CAL_COUNT; ui.cal_count = 1; break;
    case MENU_RAW: ui.page = UI_RAW; ui.info_page = 0; break;
    case MENU_INFO: ui.page = UI_INFO; ui.info_page = 0; break;
    case MENU_ID: ui_edit(EDIT_ID, ui_device_number(), 0, 999999); break;
    case MENU_DATE: ui_edit(EDIT_YEAR, saved_config.calibration_date / 10000U, 2000, 9999);
        if (ui.value == 0U) { ui.value = 2026; } break;
    case MENU_CLEAR: ui.page = UI_CONFIRM_CLEAR; break;
    case MENU_TEST: ui.page = UI_TEST; ui.info_page = 0; break;
    case MENU_SLEEP: ui.page = UI_WEIGH; enter_standby(); break;
    case MENU_EXIT: ui.page = UI_WEIGH; break;
    default: break;
    }
}

static void ui_cal_next_mass(void)
{
    static const uint16_t defaults[] = {500,2500,5000};
    ui_edit(EDIT_MASS, ui.cal_count == 1U ? 20U : defaults[ui.cal_index], 1, 5000);
}

static void ui_cal_sample(uint32_t now)
{
    const char *error = measurement_error(now, false);
    if (error != NULL) { ui_result(error, now); return; }
    if (ui.page == UI_CAL_EMPTY) {
        ui.calibration = saved_config;
        ui.calibration.zero_counts = average_counts();
        ui.calibration.point_count = 0;
        memset(ui.calibration.spans, 0, sizeof(ui.calibration.spans));
        memset(ui.calibration.grams, 0, sizeof(ui.calibration.grams));
        ui.cal_index = 0;
        ui_cal_next_mass(); return;
    }
    ui.calibration.spans[ui.cal_index] = average_counts() - ui.calibration.zero_counts;
    ui.calibration.point_count = ui.cal_index + 1U;
    if (!scale_config_valid(&ui.calibration)) { ui_message("SPAn", false, now); return; }
    if (ui.calibration.point_count < ui.cal_count) {
        ++ui.cal_index; ui_cal_next_mass(); return;
    }
    if (!save_settings(&ui.calibration)) { ui_message("FLASH", false, now); return; }
    zero_counts = saved_config.zero_counts;
    reference_grams = saved_config.grams[saved_config.point_count - 1U];
    zero_set = true; empty_reference_known = true; empty_candidate = false; tare_counts = 0;
    ui.page = UI_WEIGH;
    sample_reset();
    ui_message("SAvEd", true, now);
}

static void ui_event(KeyEvent event, uint32_t now)
{
    idle_since_ms = now;
    ui.message = NULL;
    if (ui.page == UI_WEIGH) {
        if (event == KEY_MENU) { ui.page = UI_MENU; ui.menu = MENU_UNTARE; }
        else if (event == KEY_HOLD) { ui_result(set_zero(now), now); }
        else if (event == KEY_TAP) { ui_result(set_tare(now), now); }
        else {
            ScaleConfig candidate = saved_config; candidate.unit ^= 1U;
            if (save_settings(&candidate)) { ui_message(candidate.unit ? "kg" : "g", true, now); }
            else { ui_message("FLASH", false, now); }
        }
        return;
    }
    if (event == KEY_MENU) { ui.page = UI_MENU; return; } /* 长按3秒：取消当前编辑/标定。 */
    if (ui.page == UI_EDIT) {
        if (event == KEY_HOLD) { ui_confirm_edit(now); }
        else if (event == KEY_DOUBLE) {
            unsigned digits = 1; while (power10(digits) <= ui.maximum) { ++digits; }
            ui.cursor = (ui.cursor + 1U) % digits;
        } else {
            uint32_t place = power10(ui.cursor);
            unsigned old = ui.value / place % 10U;
            ui.value = ui.value - old * place + ((old + 1U) % 10U) * place;
        }
        return;
    }
    if (event == KEY_DOUBLE) { ui.page = ui.page == UI_MENU ? UI_WEIGH : UI_MENU; return; }
    if (ui.page == UI_MENU) {
        if (event == KEY_TAP) { ui.menu = (UiMenu)((ui.menu + 1U) % MENU_COUNT); }
        else { ui_begin_item(now); }
    } else if (ui.page == UI_CHOICE) {
        if (event == KEY_TAP) { ui.choice = (ui.choice + 1U) % (ui.menu == MENU_UNIT ? 2U : 3U); }
        else {
            ScaleConfig candidate = saved_config;
            if (ui.menu == MENU_UNIT) { candidate.unit = ui.choice; }
            else { candidate.buzzer_mode = ui.choice; }
            ui_save(&candidate, now);
        }
    } else if (ui.page == UI_CAL_COUNT) {
        if (event == KEY_TAP) { ui.cal_count = ui.cal_count % 3U + 1U; }
        else { ui.page = UI_CAL_EMPTY; }
    } else if (ui.page == UI_CAL_EMPTY || ui.page == UI_CAL_LOAD) {
        if (event == KEY_HOLD) { ui_cal_sample(now); }
    } else if (ui.page == UI_CONFIRM_CLEAR && event == KEY_HOLD) {
        ScaleConfig candidate = saved_config;
        candidate.point_count = 0; candidate.zero_counts = 0;
        memset(candidate.spans, 0, sizeof(candidate.spans)); memset(candidate.grams, 0, sizeof(candidate.grams));
        if (save_settings(&candidate)) {
            zero_counts = 0; reference_grams = 0; tare_counts = 0; zero_set = false; empty_reference_known = false;
            ui.page = UI_WEIGH; ui_message("CLr", true, now);
        } else { ui_message("FLASH", false, now); }
    } else if (ui.page == UI_RAW || ui.page == UI_INFO || ui.page == UI_TEST) {
        if (event == KEY_HOLD) { ui.page = UI_MENU; }
        else { ++ui.info_page; }
    }
}

/* 30ms消抖，300ms双击窗口。短按延后确认，双击不会先执行一次去皮。 */
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
            second_press = pending && (uint32_t)(now - released) <= 300U;
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
    if (pending && !debounced && (uint32_t)(now - released) > 300U) {
        pending = false; ui_event(KEY_TAP, now);
    }
}

static void ui_render(uint32_t now)
{
    if (sleeping) { return; }
    if ((uint32_t)(now - ui.rendered_ms) < display_interval_ms) { return; }
    ui.rendered_ms = now;
    if ((uint32_t)(now - ui.startup_ms) < 2500U) { lcd_test_all(); }
    else if ((uint32_t)(now - ui.startup_ms) < 3500U) { lcd_text("vEr", SCALE_VERSION_DISPLAY, 0x18); }
    else if (ui.message != NULL && (uint32_t)(now - ui.message_ms) < 1200U) { lcd_text("", ui.message, 0); }
    else if (ui.page == UI_WEIGH) {
        weight_update(now);
        switch (weight_state.status) {
        case WEIGHT_OK: lcd_weight(weight_state.display_mg, saved_config.unit != 0U, weight_state.stable, tare_counts != 0); break;
        case WEIGHT_UNCALIBRATED: lcd_text("CAL", "no CAL", 0); break;
        case WEIGHT_SENSOR_ERROR: lcd_text("Err", "SEnSor", 0); break;
        case WEIGHT_RANGE: lcd_text("Err", "rAnGE", 0); break;
        case WEIGHT_OVERLOAD: lcd_text("Err", "  OL", 0); break;
        default: lcd_text("", "------", 0); break;
        }
    } else if (ui.page == UI_MENU) {
        /* 七段屏用数字编号，避免将长单词拼成难辨认的字形。 */
        unsigned code = (unsigned)ui.menu + 1U;
        char title[5] = {' ', ' ', (char)('0' + code / 10U), (char)('0' + code % 10U), 0};
        lcd_text(title, "", 0);
    } else if (ui.page == UI_CHOICE) {
        static const char *const beep[] = {"OFF","ACt","PASS"};
        lcd_text(ui.menu == MENU_UNIT ? "UnIt" : "bEEP", ui.menu == MENU_UNIT ? (ui.choice ? "kg" : "g") : beep[ui.choice], 0);
    } else if (ui.page == UI_EDIT) {
        static const char *const titles[] = {"OFF","StAb","rAtE","CAL","Id","YEAr","Mon","dAY"};
        unsigned low = ui.cursor >= 6U ? ui.cursor - 5U : 0U;
        uint32_t part = ui.value / power10(low);
        char text[7] = "000000";
        for (unsigned i = 0; i < 6; ++i) { text[5U - i] = (char)('0' + part % 10U); part /= 10U; }
        /* 砝码质量按实际位数显示；空白前导位不占据已知缺段的第三位。
         * 被选中的高位仍显示0，保留双击移位编辑的能力。
         */
        unsigned selected = 5U - (ui.cursor - low);
        if (ui.edit_kind == EDIT_MASS) {
            for (unsigned i = 0; i < 5U && text[i] == '0'; ++i) {
                if (i != selected) { text[i] = ' '; }
            }
        }
        if (now / 300U % 2U == 0U) { text[selected] = ' '; }
        lcd_text(titles[ui.edit_kind], text, 0);
    } else if (ui.page == UI_CAL_COUNT) { lcd_number("CAL", (int32_t)ui.cal_count, 0); }
    else if (ui.page == UI_CAL_EMPTY) { lcd_text(stable && samples_ready(now) ? "S" : "", "ZERO", 0); }
    else if (ui.page == UI_CAL_LOAD) {
        char title[5] = {'C',(char)('1' + ui.cal_index), 'g', stable && samples_ready(now) ? 'S' : ' ',0};
        lcd_number(title, (int32_t)ui.calibration.grams[ui.cal_index], 0);
    } else if (ui.page == UI_CONFIRM_CLEAR) { lcd_text("CLr", "SUrE", 0); }
    else if (ui.page == UI_RAW) {
        int32_t value = latest_raw;
        if (ui.info_page % 2U == 0U) { lcd_number(value < 0 ? "rA-L" : "rAWL", value < 0 ? -(value % 1000000) : value % 1000000, 0); }
        else { lcd_number("rAWH", value / 1000000, 0); }
    } else if (ui.page == UI_INFO) {
        switch (ui.info_page % 6U) {
        case 0: lcd_text("vEr", SCALE_VERSION_DISPLAY, 0x18); break;
        case 1: lcd_number("Id", (int32_t)ui_device_number(), 0); break;
        case 2: lcd_number("YEAr", (int32_t)(saved_config.calibration_date / 10000U), 0); break;
        case 3: lcd_number("dAtE", (int32_t)(saved_config.calibration_date % 10000U), 0); break;
        case 4: lcd_number("CAP", MAX_GROSS_GRAMS, 0); break;
        default: lcd_number("CAL", (int32_t)saved_config.point_count, 0); break;
        }
    } else if (ui.page == UI_TEST) {
        if (ui.info_page % 4U == 0U) { lcd_test_all(); }
        else if (ui.info_page % 4U == 1U) { lcd_text("1234", "123456", 0x1f); }
        else if (ui.info_page % 4U == 2U) { lcd_clear(); }
        else { lcd_number("1304", (int32_t)lcd_ram_result(), 0); }
    }
    lcd_flush();
    /* 结果页保留上一幅测试图的核对结果；全亮/数字/空白页实际读回HT1621。 */
    if (ui.page == UI_TEST && ui.info_page % 4U != 3U) { (void)lcd_verify_ram(); }
}
