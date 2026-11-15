/* 回放用户已确认空秤的实测观察序列；记录约4Hz，非完整10SPS ADC流。
 * 用途是验证漂移输入、数值计算和标定拒绝行为，不据此测量采样响应时间。
 */
static void replay_checks(void)
{
    struct { uint32_t ms; int32_t counts; } trace[256];
    FILE *f=fopen("tests/fixtures/confirmed-empty-observations.csv","r");assert(f!=NULL);
    char header[64];assert(fgets(header,sizeof(header),f)!=NULL);
    unsigned n=0;
    for(;;){
        unsigned ms;int32_t counts;
        int fields=fscanf(f,"%u,%" SCNd32 "\n",&ms,&counts);
        if(fields==EOF){break;}
        assert(fields==2 && n<256U);
        trace[n].ms=ms;trace[n].counts=counts;
        assert(n==0U || trace[n].ms>trace[n-1U].ms);++n;
    }
    fclose(f);assert(n>=32U);
    scale_config_defaults(&saved_config);
    saved_config.point_count=1;saved_config.zero_counts=trace[0].counts;
    saved_config.spans[0]=8297;saved_config.grams[0]=20;saved_config.auto_off_seconds=0;
    zero_counts=saved_config.zero_counts;reference_grams=20;tare_counts=0;stable_counts=0;
    sleeping=false;startup_zero_pending=false;empty_reference_known=true;empty_candidate=false;
    capture.kind=MEASURE_NONE;ui.page=UI_WEIGH;host_gpio_b.IDR=KEY_PIN;battery_configure(0,1200);
    sample_reset();memset(&weight_state,0,sizeof(weight_state));
    int32_t low=INT32_MAX,high=INT32_MIN;unsigned unstable=0;
    uint32_t now=10000;
    for(unsigned i=0;i<n;++i){
        now=10000U+trace[i].ms;sample_add(trace[i].counts,now);power_poll(now);
        if(weight_state.status!=WEIGHT_OK){continue;}
        if(weight_state.net_mg<low){low=weight_state.net_mg;}
        if(weight_state.net_mg>high){high=weight_state.net_mg;}
        if(!weight_state.stable){++unstable;}
    }
    assert(high-low>1500 && unstable>0U);
    assert(zero_counts-saved_config.zero_counts<=42 && zero_counts-saved_config.zero_counts>=-42);
    int32_t first=0,last=0;
    for(unsigned i=0;i<16U;++i){first+=trace[i].counts;last+=trace[n-16U+i].counts;}
    first=counts_divide(first,16);last=counts_divide(last,16);
    ScaleConfig previous=saved_config;unsigned writes=flash_program_count;
    open_item(MENU_CAL,now);ui_event(KEY_HOLD,now);ui_cal_apply(first,now);
    ui.value=20;ui_event(KEY_HOLD,now);ui_cal_apply(first+8297,now);
    assert(ui.page==UI_CAL_VERIFY);
    ui_cal_apply(last,now);
    assert(ui.page==UI_CAL_VERIFY && strcmp(ui.message,"drFt")==0);
    assert(flash_program_count==writes && memcmp(&previous,&saved_config,sizeof(previous))==0);
    ui_event(KEY_DOUBLE,now);
    printf("PASS: %u recorded empty-scale observations replayed; real drift remains visible;\n",n);
    printf("      drifted unload rejected with calibration and Flash writes unchanged.\n");
}
