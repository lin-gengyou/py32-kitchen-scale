/* V2→V3升级、电源配置、缺段绕行、蓝牙分包与长时间调度回归。 */
static void power_measure(unsigned mv, uint32_t *now)
{
    for (unsigned i=0;i<8;++i) {
        *now+=125; battery_accept_raw((mv*1000U+600U)/1200U,1000,*now);
    }
}
static void bluetooth_output(char *out, unsigned capacity)
{
    unsigned n=0; int byte;
    while ((byte=ble_test_next_tx())>=0) { assert(n+1U<capacity); out[n++]=(char)byte; }
    out[n]=0;
}
static void completion_checks(void)
{
    uint32_t now=10000;
    ScaleConfig previous, migrated;
    scale_config_defaults(&previous);
    previous.stable_counts=64;previous.point_count=1; previous.zero_counts=100000;
    previous.spans[0]=500000; previous.grams[0]=500; previous.display_interval_ms=100;
    ConfigRecord v2={.magic=CONFIG_MAGIC,.version=2,.generation=25,.config=previous};
    size_t length=offsetof(ConfigRecord,config)+offsetof(ScaleConfig,battery_profile);
    uint32_t checksum=config_crc(&v2,length);
    memset(host_flash,0xff,sizeof(host_flash));
    memcpy(host_flash,&v2,length); memcpy((uint8_t *)host_flash+length,&checksum,4);
    assert(scale_config_load(&migrated));
    assert(migrated.stable_counts==0);
    assert(migrated.point_count==1 && migrated.spans[0]==500000 && migrated.display_interval_ms==100);
    assert(migrated.battery_profile==0 && migrated.lcd_missing_mask==4 && migrated.bluetooth_enabled==1);
    assert(scale_config_save(&migrated) && scale_config_load(&previous));
    assert(memcmp(&migrated,&previous,sizeof(previous))==0);

    battery_configure(BATTERY_EXTERNAL,1200); power_measure(0,&now);
    assert(battery_state()->valid && battery_state()->percent==-1 && battery_state()->status==BATTERY_OK);
    assert(battery_state()->supply_mv==4914);
    battery_configure(BATTERY_LIION_1S,1200); power_measure(3700,&now);
    assert(battery_state()->percent>=39 && battery_state()->percent<=40);
    power_measure(3200,&now);power_measure(3200,&now);assert(battery_state()->status==BATTERY_OK);
    power_measure(3200,&now); assert(battery_state()->status==BATTERY_LOW);
    for(unsigned i=0;i<3;++i){power_measure(2900,&now);}
    assert(battery_state()->status==BATTERY_CRITICAL);
    power_measure(3050,&now); assert(battery_state()->status==BATTERY_CRITICAL);
    power_measure(3200,&now); assert(battery_state()->status==BATTERY_LOW);
    power_measure(3450,&now); assert(battery_state()->status==BATTERY_OK);
    battery_accept_raw(4096,1000,now); assert(!battery_state()->valid && battery_state()->percent==-1);
    power_measure(3700,&now); assert(battery_state()->status==BATTERY_OK);
    battery_configure(BATTERY_ALKALINE_2S,1200);power_measure(2600,&now);
    assert(battery_state()->percent==50);
    battery_configure(BATTERY_NIMH_2S,1200);power_measure(2400,&now);
    assert(battery_state()->percent==50);
    battery_poll(now,true);assert(!battery_state()->valid && battery_state()->percent==-1);
    battery_poll(now+20000,false);assert(battery_state()->status==BATTERY_WAIT);
    power_measure(2400,&now);assert(battery_state()->valid);

    saved_config=previous; reference_grams=500;zero_counts=100000;tare_counts=0;
    sleeping=false;empty_reference_known=true;ui.page=UI_WEIGH;stable_counts=64;
    saved_config.battery_profile=1;
    battery_configure(BATTERY_LIION_1S,1200);
    for(unsigned i=0;i<3;++i){power_measure(2900,&now);}
    settle(100000,&now);assert(weight_state.status==WEIGHT_LOW_BATTERY);
    assert(strcmp(set_tare(now),"ERR,POWER\r\n")==0);
    battery_accept_raw(0,0,now); weight_update(now); assert(weight_state.status==WEIGHT_POWER_ERROR);
    battery_configure(1,1200);weight_update(now);assert(weight_state.status==WEIGHT_WAIT);
    assert(strcmp(set_tare(now),"ERR,WAIT_SAMPLES\r\n")==0);
    saved_config.battery_profile=0;battery_configure(0,1200);weight_update(now);
    assert(weight_state.status==WEIGHT_OK && weight_state.battery_percent==-1);

    lcd_set_missing_mask(4);assert(lcd_body_width()==5);
    lcd_weight(5000000,false,true,false);
    assert(frame[12]==0 && frame[13]==0 && frame[20]!=0); /* 缺段位保持空白 */
    uint8_t positive[32];memcpy(positive,frame,32);
    lcd_weight(-5000000,false,true,false);
    for(unsigned i=8;i<32;++i){assert(frame[i]==positive[i]);} /* 负号移入标题 */
    assert(frame[2]!=positive[2] || frame[3]!=positive[3]);
    open_item(MENU_ID,now);ui.value=123456;ui.cursor=5;
    ui.startup_ms=now-4000;ui.rendered_ms=now-200;ui_render(now);
    assert(frame[12]==0 && frame[13]==0);
    open_item(MENU_DISPLAY,now);ui.choice=0;ui_event(KEY_HOLD,now);assert(lcd_body_width()==6);
    open_item(MENU_DISPLAY,now);ui.choice=1;ui_event(KEY_HOLD,now);assert(lcd_body_width()==5);

    char out[256];ui.page=UI_WEIGH;ble_seen=false;ble_configure(true);
    settle(200000,&now);ble_test_receive("TAR",now);bluetooth_output(out,sizeof(out));assert(out[0]==0);
    ble_test_receive("E,100\r\n",now);bluetooth_output(out,sizeof(out));
    assert(strcmp(out,"ACK,TARE,100\r\n")==0 && tare_counts==100000);
    settle(300000,&now);ble_test_receive("TARE,100\n",now);bluetooth_output(out,sizeof(out));
    assert(strcmp(out,"ACK,TARE,100\r\n")==0 && tare_counts==100000);
    sample_reset();ble_test_receive("TARE,101\r\n",now);bluetooth_output(out,sizeof(out));
    assert(strcmp(out,"ERR,TARE,101,WAIT_SAMPLES\r\n")==0);
    settle(300000,&now);ble_test_receive("TARE,101\r\n",now);bluetooth_output(out,sizeof(out));
    assert(strstr(out,"WAIT_SAMPLES") && tare_counts==100000); /* 失败结果也缓存 */
    ble_test_receive("TARE,102\r\n",now);bluetooth_output(out,sizeof(out));assert(tare_counts==200000);
    ble_test_receive("TARE,100\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"STALE_SEQ"));
    ui.page=UI_CAL_EMPTY;ble_test_receive("TARE,103\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"BUSY"));
    ui.page=UI_WEIGH;
    ble_test_receive("STATUS\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"SEEN=1,TARE_SEQ=103"));
    assert(ble_link_active(now+14999) && !ble_link_active(now+15000));
    ble_test_receive("TARE,4294967296\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"COMMAND"));
    ble_test_receive("0123456789012345678901234567890123456789\r\n",now);
    bluetooth_output(out,sizeof(out));assert(strcmp(out,"ERR,FRAME\r\n")==0);
    ble_test_receive("TA\001RE,104\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"FRAME"));
    ble_seen=false; settle(300000,&now);
    ble_test_receive("TARE,4294967295\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"ACK"));
    settle(300000,&now);ble_test_receive("TARE,0\r\n",now);bluetooth_output(out,sizeof(out));assert(strcmp(out,"ACK,TARE,0\r\n")==0);
    ble_configure(false);ble_test_receive("TARE,1\r\n",now);bluetooth_output(out,sizeof(out));assert(!*out);
    ble_configure(true);weight_state.status=WEIGHT_SENSOR_ERROR;
    ble_ring[0]='T';ble_ring[1]='A';ble_head=2;ble_rx_error=true;
    ble_poll(0,&weight_state,200);assert(ble_tail==ble_head);
    ble_test_receive("RE,1\r\n",now);bluetooth_output(out,sizeof(out));assert(strstr(out,"ERR,FRAME"));
    ble_poll(now+200,&weight_state,100);bluetooth_output(out,sizeof(out));assert(strstr(out,",,0,,-1,ERR\r\n"));
    weight_state.status=WEIGHT_OK;weight_state.display_mg=-500;weight_state.tare_mg=20000;weight_state.stable=true;
    ble_poll(now+400,&weight_state,100); /* 在重量帧中途进入应答，不允许交错 */
    int first=ble_test_next_tx();assert(first=='W');
    ble_test_receive("PING\r\n",now);bluetooth_output(out,sizeof(out));
    assert(strstr(out,",-0.5,1,20.0,-1,OK\r\nOK,PING\r\n"));

    /* 合成8小时、10Hz协议调度；每5秒心跳一次，统计完整帧，不替代实机无线长测。 */
    unsigned reports=0;now+=1000;ble_configure(true);
    for(unsigned i=0;i<288000;++i){
        now+=100;
        if(i%50U==0){ble_test_receive("PING\r\n",now);}
        ble_poll(now,&weight_state,100);bluetooth_output(out,sizeof(out));
        assert(strstr(out,"W,") && strstr(out,",OK\r\n"));++reports;
    }
    assert(reports==288000);
    /* 24小时10SPS合成采样，静态0.5g不被自动归零、不待机；遍历uint32毫秒回绕。 */
    tare_counts=0;zero_counts=100000;reference_grams=500;startup_zero_pending=true;
    ui.page=UI_WEIGH;now=UINT32_MAX-10000U;settle(100500,&now);int32_t original=zero_counts;
    for(unsigned i=0;i<864000;++i){now+=100;sample_add(100500,now);power_poll(now);}
    assert(!sleeping && zero_counts==original && weight_state.display_mg==500 && startup_zero_pending);
    settle(100000,&now);power_poll(now);assert(!startup_zero_pending);
    open_item(MENU_SOURCE,now);ui.choice=2;ui_event(KEY_HOLD,now);
    assert(saved_config.battery_profile==2);
    open_item(MENU_VREF,now);ui.value=1234;ui_event(KEY_HOLD,now);
    assert(saved_config.battery_reference_mv==1234);
    open_item(MENU_BLUETOOTH,now);ui.choice=0;ui_event(KEY_HOLD,now);
    assert(!ble_enabled && saved_config.bluetooth_enabled==0);
    open_item(MENU_RESET,now);ui_event(KEY_DOUBLE,now);assert(saved_config.point_count==1);
    open_item(MENU_RESET,now);ui_event(KEY_HOLD,now);
    assert(saved_config.point_count==0 && saved_config.battery_profile==0 &&
        saved_config.battery_reference_mv==1200 && saved_config.lcd_missing_mask==4 && ble_enabled);
    printf("PASS: V2/V3/V4 migration, ADC power profiles/hysteresis/fault gates, healthy LCD slots;\n");
    printf("      BLE fragmentation/duplicate/error/stale/wrap/heartbeat and atomic TX;\n");
    printf("      synthetic 8-hour BLE (288000 frames) and 24-hour weighing (864000 samples).\n");
}
