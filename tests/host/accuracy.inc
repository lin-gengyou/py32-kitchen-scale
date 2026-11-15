/* 验证按压偏移不会被带入标定；真实回零失败不能保存成功。 */
static void accuracy_checks(void)
{
    uint32_t now=30000;
    scale_config_defaults(&saved_config);saved_config.point_count=1;
    saved_config.zero_counts=100000;saved_config.spans[0]=500000;saved_config.grams[0]=500;
    zero_counts=100000;reference_grams=500;tare_counts=0;stable_counts=0;
    sleeping=false;capture.kind=MEASURE_NONE;battery_configure(0,1200);host_gpio_b.IDR=KEY_PIN;
    sample_reset();settle(101500,&now); /* 长按时的1.5g施力，旧算法会误存为零点。 */
    open_item(MENU_CAL,now);ui_event(KEY_HOLD,now);assert(ui.page==UI_CAL_EMPTY);
    unsigned before=flash_program_count;
    ui_event(KEY_HOLD,now);assert(capture.kind==MEASURE_CAL);
    for(unsigned i=0;i<7;++i){now+=100;sample_add(101500,now);measurement_poll(now);}
    assert(capture.count==0); /* 松手等待期完全不采入。 */
    finish_capture(100000,&now);
    assert(ui.page==UI_EDIT && ui.calibration.zero_counts==100000 && saved_config.grams[0]==500);
    ui.value=20;ui_event(KEY_HOLD,now);assert(ui.page==UI_CAL_LOAD);
    settle(121500,&now);ui_event(KEY_HOLD,now);finish_capture(120000,&now);
    assert(ui.page==UI_CAL_VERIFY && ui.calibration.spans[0]==20000 && flash_program_count==before);
    ui_event(KEY_HOLD,now);finish_capture(100500,&now); /* 留着0.5g，不能当成回零成功。 */
    assert(ui.page==UI_CAL_VERIFY && strcmp(ui.message,"drFt")==0 && flash_program_count==before);
    assert(saved_config.grams[0]==500);
    /* 保存失败不能逐次挪动草稿零点，让连续小漂移绕过最初回零检查。 */
    erase_failure=true;ui_event(KEY_HOLD,now);finish_capture(100100,&now);erase_failure=false;
    assert(ui.page==UI_CAL_VERIFY && strcmp(ui.message,"FLASH")==0);
    assert(ui.calibration.zero_counts==100000 && ui.calibration.spans[0]==20000);
    ui_event(KEY_HOLD,now);finish_capture(100300,&now);
    assert(ui.page==UI_CAL_VERIFY && strcmp(ui.message,"drFt")==0);
    ui_event(KEY_HOLD,now);finish_capture(100000,&now);
    assert(ui.page==UI_WEIGH && saved_config.grams[0]==20 && zero_counts==100000);
    settle(120000,&now);assert(weight_state.display_mg==20000);
    settle(100000,&now);assert(weight_state.display_mg==0);

    /* 去皮同样只使用释放后的容器读数，按压附加1.5g不进入皮重。 */
    settle(111500,&now);ui_event(KEY_TAP,now);assert(capture.kind==MEASURE_TARE);
    assert(strcmp(scale_remote_tare(now),"ERR,BUSY\r\n")==0);
    finish_capture(110000,&now);assert(tare_counts==10000);
    settle(130000,&now);assert(weight_state.display_mg==20000);
    settle(100000,&now);assert(weight_state.display_mg==-10000);
    tare_counts=0;

    /* 人工明确确认空秤时可修正3.8g偏移，自动追零依旧保留0.5g小负载。 */
    settle(105300,&now);ui_event(KEY_HOLD,now);finish_capture(103800,&now);
    assert(zero_counts==103800 && tare_counts==0);
    startup_zero_pending=false;empty_reference_known=true;empty_candidate=false;saved_config.auto_off_seconds=0;
    settle(103840,&now);
    for(unsigned i=0;i<140;++i){now+=100;sample_add(103840,now);power_poll(now);}
    assert(zero_counts>103800 && zero_counts<=103840 && stable);
    int32_t tracked=zero_counts;
    settle(104340,&now);
    for(unsigned i=0;i<1300;++i){now+=100;sample_add(104340,now);power_poll(now);}
    assert(zero_counts==tracked && weight_state.display_mg==500);

    /* 150mg附近的真实微噪声不会让0.1/0.2g来回跳，0.5g新负载照常响应。 */
    settle(zero_counts+140,&now);assert(weight_state.display_mg==100);
    for(unsigned i=0;i<100;++i){now+=100;sample_add(zero_counts+(i%2?145:155),now);weight_update(now);assert(weight_state.display_mg==100);}
    settle(zero_counts+500,&now);assert(weight_state.display_mg==500);
    stable=false;int32_t a=filtered_counts();stable=true;assert(filtered_counts()==a); /* 不再切换均值。 */
    const int32_t negative[]={-100,-101,-102,-103,-104};
    assert(trimmed_mean(negative,5,NULL)==-102); /* 防止负数除以unsigned而变成巨值。 */
    static const int32_t signed_cases[]={INT32_MIN,-8388608,-1234567,-1,0,1,8388607,INT32_MAX};
    static const uint32_t positive_divisors[]={1,3,6,14,100000,1000000};
    for(unsigned i=0;i<sizeof(signed_cases)/sizeof(signed_cases[0]);++i){
        for(unsigned j=0;j<sizeof(positive_divisors)/sizeof(positive_divisors[0]);++j){
            assert(counts_divide(signed_cases[i],positive_divisors[j])==(int32_t)((int64_t)signed_cases[i]/positive_divisors[j]));
        }
    }
    uint32_t unit=saved_config.unit;
    ui_event(KEY_TAP,now);assert(capture.kind==MEASURE_TARE);
    ui_event(KEY_DOUBLE,now);assert(capture.kind==MEASURE_NONE && saved_config.unit==unit);
    /* 采到一半遇到ADC故障，恢复后必须整组重采，不能混入故障前的旧样本。 */
    ui_event(KEY_TAP,now);
    for(unsigned i=0;i<15;++i){now+=100;sample_add(zero_counts+500,now);measurement_poll(now);}
    assert(capture.count>0 && capture.count<16);
    sample_fault=true;measurement_poll(now+1);assert(capture.count==0);
    finish_capture(zero_counts+800,&now);assert(tare_counts==800);
    int32_t old_tare=tare_counts;
    ui_event(KEY_TAP,now);
    for(unsigned i=0;i<200U;++i){now+=100;sample_add(zero_counts+(i%2U ? 3000 : -3000),now);measurement_poll(now);}
    measurement_poll(++now);
    assert(capture.kind==MEASURE_NONE && tare_counts==old_tare && strcmp(ui.message,"UnSt")==0);
    ui_event(KEY_TAP,now);sample_fault=true;measurement_poll(now+20001U);
    assert(capture.kind==MEASURE_NONE && tare_counts==old_tare && strcmp(ui.message,"no Ad")==0);
    sample_fault=false;
    printf("PASS: post-release 16-sample acquisition, button force rejection, unload verification/rollback;\n");
    printf("      0/20g/0 calibration, async tare/zero/cancel/timeout, fractional zero tracking,\n");
    printf("      quantizer hysteresis, negative signed mean, and 0.5g preservation.\n");
}
