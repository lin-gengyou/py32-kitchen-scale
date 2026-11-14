/* ECB01C的9600/8N1透传。RX中断仅入环形队列，主循环组帧、执行和发送。
 * TARE序号重传返回同一结果，不再次去皮；失败结果也参与去重。
 */
#include "ble_link.h"
#include "py32f0xx_hal.h"
#include <string.h>

static bool ble_enabled, ble_seen, ble_peer;
static uint32_t ble_last_sequence, ble_last_peer, ble_report_ms, ble_stream_sequence;
static char ble_last_reply[48], ble_line[32], ble_replies[2][80], ble_stream[96];
static uint8_t ble_length, ble_reply_head, ble_reply_tail, ble_reply_count;
static bool ble_invalid, ble_stream_ready, ble_active_reply;
static char *ble_active;
static volatile uint8_t ble_ring[64], ble_head, ble_tail;
static volatile bool ble_rx_error;
static char *ble_out, *ble_end;

static void ble_begin(char *out, unsigned capacity) { ble_out=out; ble_end=out+capacity-1U; *ble_out=0; }
static void ble_text(const char *s)
{
    while (*s && ble_out<ble_end) { *ble_out++=*s++; } *ble_out=0;
}
static void ble_unsigned(uint32_t value)
{
    char digits[10]; unsigned count=0;
    do { digits[count++]=(char)('0'+value%10U); value/=10U; } while(value);
    while(count && ble_out<ble_end) { *ble_out++=digits[--count]; } *ble_out=0;
}
static void ble_grams(int32_t mg)
{
    uint32_t absolute=mg<0 ? (uint32_t)(-(int64_t)mg) : (uint32_t)mg;
    if(mg<0){ble_text("-");} absolute=(absolute+50U)/100U;
    ble_unsigned(absolute/10U); ble_text("."); ble_unsigned(absolute%10U);
}
static char *ble_reply(void)
{
    if(ble_reply_count==2U){return NULL;}
    char *result=ble_replies[ble_reply_head];
    ble_reply_head^=1U; ++ble_reply_count; ble_begin(result,80); return result;
}
static bool ble_sequence(const char *text,uint32_t *value)
{
    if(!*text){return false;} uint32_t n=0;
    while(*text){unsigned d=(unsigned)(*text++-'0');if(d>9U || n>(UINT32_MAX-d)/10U){return false;}n=n*10U+d;}
    *value=n;return true;
}
static void ble_command(uint32_t now)
{
    if(ble_reply_count==2U){return;} /* 客户端收到应答后才下发下一条。 */
    char *reply=ble_reply(); if(reply==NULL){return;}
    ble_peer=true; ble_last_peer=now;
    if(ble_invalid){ble_text("ERR,FRAME\r\n");return;}
    if(strcmp(ble_line,"PING")==0){ble_text("OK,PING\r\n");return;}
    if(strcmp(ble_line,"WAKE")==0){scale_remote_wake(now);ble_text("OK,WAKE\r\n");return;}
    if(strcmp(ble_line,"STATUS")==0){
        ble_text("S,SEEN=");ble_unsigned(ble_seen);ble_text(",TARE_SEQ=");ble_unsigned(ble_last_sequence);ble_text("\r\n");return;
    }
    uint32_t sequence;
    if(strncmp(ble_line,"TARE,",5)!=0 || !ble_sequence(ble_line+5,&sequence)){
        ble_text("ERR,COMMAND\r\n");return;
    }
    if(ble_seen && sequence==ble_last_sequence){ble_text(ble_last_reply);return;}
    uint32_t difference=sequence-ble_last_sequence;
    if(ble_seen && (difference==0U || difference>=0x80000000U)){
        ble_text("ERR,TARE,");ble_unsigned(sequence);ble_text(",STALE_SEQ\r\n");return;
    }
    const char *result=scale_remote_tare(now);
    bool ok=result[0]=='O';
    ble_text(ok ? "ACK,TARE," : "ERR,TARE,");ble_unsigned(sequence);
    if(!ok){ble_text(",");const char *p=result+4;while(*p && *p!='\r' && ble_out<ble_end){*ble_out++=*p++;}*ble_out=0;}
    ble_text("\r\n");
    ble_seen=true;ble_last_sequence=sequence;
    /* 最长回复小于48字节。保留完整终止符，重试不改变测量也不再次蜂鸣。 */
    unsigned length=(unsigned)strlen(reply);
    if(length<sizeof(ble_last_reply)){memcpy(ble_last_reply,reply,length+1U);}
}
static void ble_receive(uint8_t byte,uint32_t now)
{
    if(byte=='\r' || byte=='\n'){
        if(ble_length || ble_invalid){ble_line[ble_length]=0;ble_command(now);}
        ble_length=0;ble_invalid=false;
    }else if(byte<32U || byte>126U || ble_length==sizeof(ble_line)-1U){ble_invalid=true;}
    else if(!ble_invalid){ble_line[ble_length++]=(char)byte;}
}
static int ble_next_tx(void)
{
    if(ble_active==NULL){
        if(ble_reply_count){ble_active=ble_replies[ble_reply_tail];ble_active_reply=true;}
        else if(ble_stream_ready){ble_active=ble_stream;ble_active_reply=false;}
        else{return -1;}
    }
    int byte=(uint8_t)*ble_active++;
    if(*ble_active==0){
        if(ble_active_reply){ble_reply_tail^=1U;--ble_reply_count;}else{ble_stream_ready=false;}
        ble_active=NULL;
    }
    return byte;
}

void ble_configure(bool enabled)
{
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    ble_enabled=enabled;ble_active=NULL;ble_stream_ready=false;ble_length=0;
    ble_reply_count=ble_reply_head=ble_reply_tail=0;ble_invalid=false;
    ble_head=ble_tail=0;ble_rx_error=false;ble_peer=false;
#ifndef SCALE_HOST_TEST
    __HAL_RCC_USART1_CLK_ENABLE(); USART1->CR1=0;
    GPIO_InitTypeDef pin={.Pin=GPIO_PIN_2|GPIO_PIN_3,.Mode=enabled?GPIO_MODE_AF_PP:GPIO_MODE_INPUT,
        .Pull=GPIO_NOPULL,.Speed=GPIO_SPEED_FREQ_LOW,.Alternate=GPIO_AF1_USART1};
    HAL_GPIO_Init(GPIOA,&pin);
    if(enabled){
        USART1->CR2=0;USART1->CR3=0;USART1->BRR=(HAL_RCC_GetPCLK1Freq()+4800U)/9600U;
        HAL_NVIC_SetPriority(USART1_IRQn,2,0);HAL_NVIC_EnableIRQ(USART1_IRQn);
        USART1->CR1=USART_CR1_TE|USART_CR1_RE|USART_CR1_RXNEIE|USART_CR1_UE;
    }else{HAL_NVIC_DisableIRQ(USART1_IRQn);}
#endif
    __set_PRIMASK(mask);
}
void ble_uart_irq(void)
{
#ifndef SCALE_HOST_TEST
    unsigned flags=USART1->SR;
    if(flags&(USART_SR_RXNE|USART_SR_ORE|USART_SR_NE|USART_SR_FE|USART_SR_PE)){
        uint8_t byte=(uint8_t)USART1->DR;
        uint8_t next=(ble_head+1U)&63U;
        if((flags&(USART_SR_ORE|USART_SR_NE|USART_SR_FE|USART_SR_PE)) || next==ble_tail){ble_rx_error=true;}
        else{ble_ring[ble_head]=byte;ble_head=next;}
    }
#endif
}
bool ble_link_active(uint32_t now){return ble_enabled && ble_peer && (uint32_t)(now-ble_last_peer)<15000U;}
void ble_poll(uint32_t now,const WeightState *w,uint32_t period_ms)
{
    if(!ble_enabled){return;}
    /* 错误标志与取字节一起检查，防止ISR刚报告丢字节时主循环仍执行残缺指令。 */
    for(;;){
        uint32_t mask=__get_PRIMASK();__disable_irq();
        bool bad=ble_rx_error;
        if(bad){ble_tail=ble_head;ble_rx_error=false;}
        bool available=ble_tail!=ble_head;uint8_t byte=0;
        if(available){byte=ble_ring[ble_tail];ble_tail=(ble_tail+1U)&63U;}
        __set_PRIMASK(mask);
        if(bad){ble_length=0;ble_invalid=true;}
        if(!available){break;}
        ble_receive(byte,now);
    }
    if(!ble_stream_ready && (uint32_t)(now-ble_report_ms)>=period_ms){
        ble_report_ms=now;ble_begin(ble_stream,sizeof(ble_stream));
        ble_text("W,");ble_unsigned(ble_stream_sequence++);ble_text(",");
        bool good=w->status==WEIGHT_OK;
        if(good){ble_grams(w->display_mg);}ble_text(",");ble_unsigned(good&&w->stable);ble_text(",");
        if(good){ble_grams(w->tare_mg);}ble_text(",");
        if(w->battery_percent<0){ble_text("-1");}else{ble_unsigned((unsigned)w->battery_percent);}
        static const char *const status[]={"WAIT","UNCAL","OK","RANGE","OL","ERR","LOWBAT","POWERERR","CALIBRATING","SLEEP"};
        ble_text(",");ble_text(status[w->status]);ble_text("\r\n");ble_stream_ready=true;
    }
#ifndef SCALE_HOST_TEST
    if(USART1->SR&USART_SR_TXE){int byte=ble_next_tx();if(byte>=0){USART1->DR=(uint8_t)byte;}}
#endif
}
#ifdef SCALE_HOST_TEST
void ble_test_receive(const char *text,uint32_t now){if(!ble_enabled){return;}while(*text){ble_receive((uint8_t)*text++,now);}}
int ble_test_next_tx(void){return ble_next_tx();}
#endif
