/* 稳定显示锁定的用户场景：内部计数继续变化，屏幕/BLE读数保持，动作即解锁。 */
static void hold_checks(void)
{
    uint32_t now=10000;
    scale_config_defaults(&saved_config);
    saved_config.auto_off_seconds=0;
    saved_config.point_count=1;saved_config.spans[0]=20000;saved_config.grams[0]=20;
    saved_config.zero_counts=100000;zero_counts=100000;reference_grams=20;
    tare_counts=0;stable_counts=0;sleeping=false;capture.kind=MEASURE_NONE;
    battery_configure(0,1200);ui.page=UI_WEIGH;host_gpio_b.IDR=KEY_PIN;
    sample_reset();settle(100000,&now);
    static const int32_t noise[]={-80,-20,60,100,40,0};
    bool moved=false;
    for(unsigned i=0;i<180U;++i){
        now+=100;sample_add(100000+noise[i%6],now);weight_update(now);
        assert(weight_state.stable && weight_state.display_mg==0);
        moved|=weight_state.net_mg!=0;
    }
    assert(moved);
    /* 空盘确认后随慢漂保持零，0.5g退出慢漂跟踪，示值继续为零。 */
    assert(zero_at(100000,now) == MEAS_OK);
    for (unsigned i=0;i<150U;++i) {
        now+=100;sample_add(100000+(int32_t)i*10,now);power_poll(now);
        assert(zero_locked && weight_state.display_mg==0 && weight_state.stable);
    }
    assert(zero_counts>101400 && saved_config.zero_counts==100000);
    int32_t base=zero_counts;
    settle(base+500,&now);
    assert(!zero_locked && weight_state.net_mg>=450 && weight_state.net_mg<=550 && weight_state.display_mg==0);
    zero_counts=100000;
    sample_reset();
    /* 直接漂至±9g始终显示零，12g以内继续锁零，正常加载与卸载使用滞回。 */
    settle(102500,&now);assert(weight_state.net_mg==2500 && weight_state.display_mg==0);
    settle(91000,&now);assert(weight_state.net_mg==-9000 && weight_state.display_mg==0);
    settle(112000,&now);assert(weight_state.display_mg==0);
    settle(112500,&now);assert(weight_state.display_mg==12500);
    settle(111000,&now);assert(weight_state.display_mg==11000);
    settle(110000,&now);assert(weight_state.display_mg==0);

    settle(120000,&now);assert(weight_state.display_mg==20000);
    for(unsigned i=0;i<180U;++i){
        now+=100;sample_add(120060+(int32_t)(i%6U)*10,now);weight_update(now);
        assert(weight_state.stable && weight_state.display_mg==20000);
    }
    assert(weight_state.net_mg>20075); /* 已超过旧版3/4分度更新点，仍应锁在20.0。 */
    settle(120240,&now);assert(weight_state.display_mg==20000);
    bool unlocked=false;
    for(unsigned i=1;i<=160U;++i){
        now+=100;sample_add(120155+(int32_t)i*5,now);weight_update(now);
        unlocked|=!weight_state.stable;
    }
    assert(unlocked && weight_state.display_mg>=20500);
    settle(100000,&now);assert(weight_state.display_mg==0);

    /* 实际输出循环没观察到中间的不稳定帧，也不能沿用上一物品的锁定值。 */
    settle(120000,&now);
    for(unsigned i=0;i<24U;++i){now+=100;sample_add(121000,now);}
    weight_update(now);assert(weight_state.stable && weight_state.display_mg==21000);

    /* 手动归零、去皮、取消去皮都立即改变输出基准；稳定负净重也能锁定。 */
    assert(zero_at(121000,now) == MEAS_OK);
    assert(weight_state.display_mg==0);
    settle(131000,&now);assert(tare_at(131000,now) == MEAS_OK);
    settle(151000,&now);assert(weight_state.display_mg==20000);
    open_item(MENU_GROSS,now);assert(weight_state.display_mg==30000 && tare_counts==10000);
    open_item(MENU_GROSS,now);assert(weight_state.display_mg==20000 && tare_counts==10000);
    assert(tare_at(151000,now) == MEAS_OK);
    settle(136000,&now);assert(weight_state.display_mg==-15000);
    settle(121000,&now);assert(weight_state.display_mg==0 && tare_counts==30000);
    settle(136000,&now);assert(weight_state.display_mg==-15000);
    for(unsigned i=0;i<80U;++i){
        now+=100;sample_add(136000+noise[i%6],now);weight_update(now);
        assert(weight_state.stable && weight_state.display_mg==-15000);
    }
    sample_fault=true;weight_update(now);
    assert(weight_state.status==WEIGHT_SENSOR_ERROR && !weight_state.stable);
    sample_reset();settle(136000,&now);assert(weight_state.display_mg==-15000);

    /* 进入稳定时以最新均值取最近分度；分度切换后仍按新重量锁定。 */
    tare_counts=0;zero_counts=100000;sample_reset();settle(120060,&now);
    assert(weight_state.display_mg==20100);
    settle(300800,&now);assert(weight_state.display_mg==201000 && weight_state.division_mg==500);
    settle(1101200,&now);assert(weight_state.display_mg==1001000 && weight_state.division_mg==1000);
    settle(5350000,&now);assert(weight_state.status==WEIGHT_OVERLOAD && !weight_state.stable);
    sample_reset();settle(100000,&now);assert(weight_state.display_mg==0);
    printf("PASS: stable zero/load/negative display hold with live internal readings;\n");
    printf("      10g/12g zero hysteresis, slow filling, missed output frames, tare/zero, fault and division changes.\n");
}
