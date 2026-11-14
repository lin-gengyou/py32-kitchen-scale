import {LineParser, CommandChannel, statuses} from './protocol.mjs';
const $ = id => document.getElementById(id);
const errors = {UNSTABLE:'秤台尚未稳定', WAIT_SAMPLES:'正在等待新鲜采样', OVERLOAD:'先移走超量程负载',
  NEED_CAL:'先用秤上菜单07完成标定', BUSY:'秤上正在标定', POWER:'电池电压低或采样异常', STALE_SEQ:'去皮序号已过期，请重连同步'};
let device, notify, writer, channel, ready = false, stopped = true, retryTimer, heartbeat;
let generation = 0;
let sequence = 0, lastFrame = 0, latest, count = 0, attempts = 0, demoTimer, demoTare = 0, demoMass = 120.5;
const parser = new LineParser(frame => {
  channel?.accept(frame);
  if (frame.type !== 'weight') return;
  latest = frame; lastFrame = Date.now(); count++; render();
});
function message(text) { $('message').textContent = text; }
function render() {
  const fresh = latest && Date.now() - lastFrame < 3000;
  const good = fresh && latest.status === 'OK';
  $('state').textContent = fresh ? statuses[latest.status] : ready ? '等待重量数据' : '连接你的电子秤';
  $('stability').textContent = good ? latest.stable ? '● 稳定' : '◌ 变化中' : '—';
  $('tare-info').textContent = good ? `皮重 ${latest.tare.toFixed(1)} g` : '皮重 —';
  $('power').textContent = fresh && latest.battery >= 0 ? `电池 ${latest.battery}%` : '外部供电 / 电量未知';
  const unit = $('units').value; let value = '—';
  if (good) {
    const n = latest.net;
    if (unit === 'g') value = n.toFixed(1);
    else if (unit === 'kg') value = (n / 1000).toFixed(4);
    else if (unit === 'oz') value = (n / 28.349523125).toFixed(3);
    else { const oz = Math.round(Math.abs(n) / 28.349523125 * 100) / 100;
      value = `${n < 0 ? '−' : ''}${Math.floor(oz / 16)}:${(oz % 16).toFixed(2).padStart(5,'0')}`; }
  }
  $('weight').textContent = value; $('unit').textContent = unit === 'lb' ? 'lb:oz' : unit;
  $('tare').disabled = !(good && latest.stable && ready) || !!channel?.pending;
  $('wake').disabled = !ready || !!channel?.pending;
  $('disconnect').disabled = !device && !demoTimer;
  $('counter').textContent = count ? `已接收 ${count} 帧` : '';
}
function uuid(id) {
  const text = $(id).value.trim().toLowerCase();
  if (/^[0-9a-f]{4}$/.test(text)) return Number.parseInt(text,16);
  if (/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(text)) return text;
  throw new Error('UUID请填写4位十六进制或完整128位UUID');
}
async function write(text) {
  if (!writer || !device?.gatt.connected) throw new Error('蓝牙已断开');
  const data = new TextEncoder().encode(text);
  for (let at=0;at<data.length;at+=20) {
    const block = data.slice(at,at+20);
    if (writer.properties.write) await writer.writeValueWithResponse(block);
    else await writer.writeValueWithoutResponse(block);
  }
}
const onNotify = event => parser.push(new Uint8Array(event.target.value.buffer,
  event.target.value.byteOffset,event.target.value.byteLength));
function cleanLink() {
  ready=false; latest=null; clearInterval(heartbeat); channel?.abort();
  notify?.removeEventListener('characteristicvaluechanged',onNotify);
  notify=null;writer=null;parser.reset();render();
}
async function openGatt() {
  const current = device, token = generation;
  const active = () => token === generation && !stopped;
  try {
    $('connection').textContent = '正在连接';
    const server = await current.gatt.connect();
    if (!active()) { server.disconnect(); return; }
    // 每次连接重新获取句柄；断线前的服务/特性对象已经失效。
    const service = await server.getPrimaryService(uuid('service'));
    if (!active()) return;
    const nextNotify = await service.getCharacteristic(uuid('notify'));
    if (!active()) return;
    notify = nextNotify;
    const nextWriter = await service.getCharacteristic(uuid('write'));
    if (!active()) return;
    writer = nextWriter;
    parser.reset();channel = new CommandChannel(write);
    notify.addEventListener('characteristicvaluechanged',onNotify);
    await notify.startNotifications();
    if (!active()) return;
    const state = await channel.request('STATUS',f=>f.type==='state');
    if (!active()) return;
    sequence = state.seen ? (state.seq+1)>>>0 : 0;
    ready=true;attempts=0;$('connection').textContent=device.name || '蓝牙秤';
    message('已连接。等待稳定后，可去皮称重。');render();
    heartbeat=setInterval(()=>{
      if (ready && !channel.pending) channel.request('PING',f=>f.type==='reply'&&f.command==='PING')
        .catch(e=>{message(e.message);if(device?.gatt.connected)device.gatt.disconnect();});
    },5000);
  } catch (e) {
    if (!active()) return;
    cleanLink();message(e.message);
    if (device?.gatt.connected) device.gatt.disconnect();
    scheduleReconnect();
  }
}
function scheduleReconnect() {
  if(stopped || retryTimer) return;
  const delay=Math.min(1000*2**Math.min(attempts++,4),15000);
  $('connection').textContent='断线，正在重连';
  retryTimer=setTimeout(()=>{retryTimer=null;openGatt();},delay);
}
function onDisconnected() { cleanLink();scheduleReconnect(); }
function stop() {
  generation++;stopped=true;clearTimeout(retryTimer);retryTimer=null;clearInterval(demoTimer);demoTimer=null;
  if(device) {device.removeEventListener('gattserverdisconnected',onDisconnected);device.gatt.disconnect();}
  cleanLink();device=null;$('connection').textContent='未连接';render();
}
$('connect').onclick=async()=>{
  stop();
  try {
    if(!window.isSecureContext || !navigator.bluetooth) throw new Error('请在localhost或HTTPS页面使用支持Web Bluetooth的Chrome / Edge');
    const service=uuid('service');uuid('notify');uuid('write');
    const token=generation;
    const selected=await navigator.bluetooth.requestDevice({acceptAllDevices:true,optionalServices:[service]});
    if(token!==generation)return;
    device=selected;
    stopped=false;device.addEventListener('gattserverdisconnected',onDisconnected);await openGatt();
  } catch(e) {message(e.message);}
};
$('disconnect').onclick=()=>{stop();message('已断开。板上按键可继续独立称重。');};
$('tare').onclick=async()=>{
  if(demoTimer){demoTare=demoMass;message('演示：已去皮');return;}
  if(!ready || !latest?.stable || latest.status!=='OK')return;
  try {
    const seq=sequence;sequence=(sequence+1)>>>0;
    const pending=channel.request(`TARE,${seq}`,f=>f.type==='tare'&&f.seq===seq);render();
    const reply=await pending;
    message(reply.ok?'去皮完成':errors[reply.reason] || reply.reason);
  } catch(e){message(e.message);}finally{render();}
};
$('wake').onclick=async()=>{
  if(demoTimer)return;
  try{const pending=channel.request('WAKE',f=>f.type==='reply'&&f.command==='WAKE');render();await pending;message('已唤醒，正在积累采样');}
  catch(e){message(e.message);}finally{render();}
};
$('units').onchange=render;
$('demo').onclick=()=>{
  stop();ready=true;demoTare=0;let t=0;channel=null;
  $('connection').textContent='演示模式';message('演示数据，可体验去皮和单位切换；实际称重请点击连接蓝牙秤。');
  demoTimer=setInterval(()=>{t+=.2;demoMass=120.5+Math.floor(t/8)*20;
    latest={status:'OK',net:demoMass-demoTare,tare:demoTare,stable:true,battery:-1};lastFrame=Date.now();count++;render();},200);
};
setInterval(render,500);render();
