/* 独立驱动仿真：逐个SCK边沿推出DOUT，覆盖实际hx711_read/sample_poll，
 * 与直接喂sample_add的滤波测试分开；仿真不等同于示波器测量物理边沿。
 */
static bool host_hx_mock, host_hx_record, host_hx_ready, host_hx_stuck_low;
static uint32_t host_hx_word;
static unsigned host_hx_pulses, host_hx_edges;
static bool host_hx_edge[50];

void host_hx711_clock(bool high)
{
    HAL_GPIO_WritePin(GPIOA,GPIO_PIN_6,high ? GPIO_PIN_SET : GPIO_PIN_RESET);
    if (!host_hx_mock || !host_hx_record) { return; }
    assert(host_primask==1U && host_hx_edges<50U);
    host_hx_edge[host_hx_edges++]=high;
    if(high){++host_hx_pulses;assert(host_hx_pulses<=25U);}
    if(host_hx_edges==50U){host_hx_record=false;}
}

bool host_hx711_data(void)
{
    if(!host_hx_mock){return (host_gpio_a.IDR&GPIO_PIN_0)!=0U;}
    if(host_hx_pulses==0U){return !host_hx_ready;}
    if(host_hx_pulses<=24U){return ((host_hx_word>>(24U-host_hx_pulses))&1U)!=0U;}
    return !host_hx_stuck_low;
}

static void hx_prepare(uint32_t word,bool ready,bool stuck)
{
    host_hx_mock=host_hx_record=true;
    host_hx_word=word;host_hx_ready=ready;host_hx_stuck_low=stuck;
    host_hx_pulses=host_hx_edges=0;
}

static void hx_edges_check(void)
{
    assert(host_hx_edges==50U && host_hx_pulses==25U);
    for(unsigned i=0;i<50U;++i){assert(host_hx_edge[i]==(i%2U==0U));}
    assert((host_gpio_a.ODR&GPIO_PIN_6)==0U);
}

static void hx711_protocol_checks(void)
{
    static const uint32_t words[]={0,1,0x123456,0x7ffffe,0x800001,0xffffff};
    static const int32_t expected[]={0,1,0x123456,8388606,-8388607,-1};
    for(unsigned mask=0;mask<=1U;++mask){
        for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i){
            hx_prepare(words[i],true,false);host_primask=mask;
            int32_t value=123;
            assert(hx711_read(&value) && value==expected[i] && !adc_saturated);
            assert(host_primask==mask);hx_edges_check();
        }
    }
    host_primask=0;
    for(unsigned i=0;i<2U;++i){
        hx_prepare(i ? 0x7fffff : 0x800000,true,false);int32_t value=123;
        assert(!hx711_read(&value) && adc_saturated && value==123);
        hx_edges_check();
    }
    hx_prepare(0x123456,true,true);int32_t value=123;
    assert(!hx711_read(&value) && value==123 && !adc_saturated);hx_edges_check();

    /* 启动复位：高电平至少1ms，下降沿后不足500ms不读任何旧转换。 */
    scale_config_defaults(&saved_config);stable_counts=0;sleeping=false;
    sample_fault=adc_saturated=false;host_hx_record=false;
    uint32_t restarts=hx711_restarts;hx711_restart(1000);
    assert(hx711_restarts==restarts+1U);
    assert(hx711_resetting && (host_gpio_a.ODR&GPIO_PIN_6) && sample_count==0);
    hx_prepare(10000,true,false);host_hx_record=false;
    sample_poll(1000);assert(hx711_resetting && host_hx_pulses==0);
    sample_poll(1001);assert(hx711_resetting && (host_gpio_a.ODR&GPIO_PIN_6));
    sample_poll(1002);assert(!hx711_resetting && !(host_gpio_a.ODR&GPIO_PIN_6));
    sample_poll(1501);assert(!hx711_started && host_hx_pulses==0);
    hx_prepare(10000,true,false);sample_poll(1502);
    assert(hx711_started && latest_raw==10000 && raw_count==1 && !samples_ready(1502));hx_edges_check();

    /* 读完后DOUT不释放：硬复位并丢弃旧窗口，再重新收满有效样本。 */
    hx_prepare(10001,true,true);sample_poll(1601);
    assert(sample_fault && hx711_resetting && sample_count==0 && raw_count==0);
    sample_poll(1602);assert(hx711_resetting);
    sample_poll(1603);
    hx_prepare(20000,true,false);sample_poll(2102);assert(host_hx_pulses==0);
    for(unsigned i=0;i<5U;++i){hx_prepare(20000,true,false);sample_poll(2103U+i*100U);}
    assert(!sample_fault && samples_ready(2503) && sample_count==3 && latest_raw==20000);
    for(unsigned i=0;i<3U;++i){assert(raw_samples[i]==20000 && samples[i]==20000);}

    /* 卡高/断线不阻塞主循环；每次超时都重新同步，而非永远沿用错帧状态。 */
    hx_prepare(0,false,false);host_hx_record=false;
    sample_poll(3503);assert(sample_fault && hx711_resetting && sample_count==0);
    sample_poll(3504);assert(hx711_resetting);
    sample_poll(3505);sample_poll(4005);assert(!hx711_resetting && hx711_started && host_hx_pulses==0);
    sample_poll(4505);assert(hx711_resetting && host_hx_pulses==0);

    /* 毫秒回绕，以及待机唤醒后的500ms建立等待。 */
    hx711_restart(UINT32_MAX-5U);sample_poll(0);assert(!hx711_resetting && started_ms==0);
    hx_prepare(30000,true,false);sample_poll(499);assert(host_hx_pulses==0);
    sample_poll(500);assert(latest_raw==30000);
    sleeping=true;host_hx_record=false;HX711_CLOCK(true);hx711_resetting=true;
    wake_scale(1000);assert(!sleeping && !hx711_resetting && !hx711_started);
    hx_prepare(40000,true,false);sample_poll(1499);assert(host_hx_pulses==0);
    sample_poll(1500);assert(latest_raw==40000 && raw_count==1);

    host_hx_mock=host_hx_record=false;sample_reset();sample_fault=adc_saturated=false;
    hx711_started=hx711_resetting=false;started_ms=last_sample_ms=0;host_primask=0;
    printf("PASS: HX711 25-pulse bit protocol, signed extremes, IRQ restore and invalid frames;\n");
    printf("      startup/recovery reset, 500ms settling, timeout retry, wake and tick wrap.\n");
}
