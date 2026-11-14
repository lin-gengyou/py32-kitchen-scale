/* 传感器噪声、真实阶跃与慢加料分别覆盖，S的保持不能靠冻结重量实现。 */
static void stability_checks(void)
{
    uint32_t now=0;
    scale_config_defaults(&saved_config);
    saved_config.point_count=1;saved_config.spans[0]=8973;saved_config.grams[0]=20;
    saved_config.zero_counts=100000;zero_counts=100000;reference_grams=20;
    tare_counts=0;sleeping=false;stable_counts=0;battery_configure(0,1200);
    assert(stability_limit()==90);
    saved_config.spans[0]=-8973;assert(stability_limit()==90);saved_config.spans[0]=8973;
    sample_reset();settle(100000,&now);
    static const int32_t noise[]={-35,-25,5,35,25,-5};
    for(unsigned i=0;i<180;++i){
        now+=100;sample_add(100000+noise[i%6],now);weight_update(now);
        assert(stable);
    }
    /* 偶发的±70计数扰动超过进入门限，仍小于180退出门限；均值无实际漂移。 */
    for(unsigned i=0;i<120;++i){
        int32_t n=i%20U<3U ? 70 : i%20U<6U ? -70 : 0;
        now+=100;sample_add(100000+n,now);weight_update(now);assert(stable);
    }
    settle(100040,&now);assert(stable && weight_state.display_mg==100); /* S不锁住显示 */
    unsigned count=0;
    while(stable && count<4U){now+=100;sample_add(100264,now);++count;}
    assert(!stable && count<=2U); /* 真实0.5g变化在中值确认后立即退出 */
    settle(100264,&now);assert(stable);
    bool exited=false;
    for(unsigned i=1;i<=80;++i){
        now+=100;sample_add(100264+(int32_t)i*2,now);weight_update(now);
        if(!stable){exited=true;}
    }
    assert(exited && weight_state.display_mg>=800); /* 慢加料也不能永久保持旧锚点 */
    stable_counts=10;assert(stability_limit()==10);sample_reset();
    for(unsigned i=0;i<50;++i){now+=100;sample_add(100000+noise[i%6],now);}
    assert(!stable);settle(100000,&now);assert(stable);
    saved_config.point_count=0;stable_counts=0;assert(stability_limit()==128);

    ScaleConfig old,migrated;scale_config_defaults(&old);old.stable_counts=64;
    ConfigRecord record={.magic=CONFIG_MAGIC,.version=3,.generation=5,.config=old};
    record.crc=config_crc(&record,offsetof(ConfigRecord,crc));
    assert(record_read(&record,&migrated) && migrated.stable_counts==0);
    record.config.stable_counts=100;record.crc=config_crc(&record,offsetof(ConfigRecord,crc));
    assert(record_read(&record,&migrated) && migrated.stable_counts==100);
    record.version=4;record.config.stable_counts=64;record.crc=config_crc(&record,offsetof(ConfigRecord,crc));
    assert(record_read(&record,&migrated) && migrated.stable_counts==64);
    printf("PASS: calibrated 0.2g automatic stability, exit hysteresis, live display, 0.5g step, slow ramp;\n");
    printf("      manual tolerance, uncalibrated startup, V3 default migration and V4 manual preservation.\n");
}
