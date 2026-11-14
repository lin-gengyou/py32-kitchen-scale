import {createRequire} from 'node:module';
import {createServer} from 'node:http';
import {readFile, mkdir} from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'playwright');
await mkdir('logs',{recursive:true});
const root=path.resolve('web');
const server=createServer(async(req,res)=>{try{let p=decodeURIComponent(req.url.split('?')[0]);if(p==='/')p='/index.html';const file=path.resolve(root,'.'+p);if(!file.startsWith(root+path.sep))throw Error();res.setHeader('Content-Type',p.endsWith('.mjs')?'text/javascript':p.endsWith('.css')?'text/css':'text/html; charset=utf-8');res.end(await readFile(file));}catch{res.statusCode=404;res.end();}});
await new Promise(r=>server.listen(18765,'127.0.0.1',r));
let browser;
try{
 browser=await chromium.launch({channel:'msedge',headless:true});
 const page=await browser.newPage({viewport:{width:1060,height:850}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.addInitScript(()=>{
  let seq=0,connects=0,gets=0,command='',tareCalls=0;
  const notify=new EventTarget();notify.startNotifications=async()=>notify;
  function emit(t){for(let i=0;i<t.length;i+=7){const b=new TextEncoder().encode(t.slice(i,i+7));notify.value=new DataView(b.buffer);notify.dispatchEvent(new Event('characteristicvaluechanged'));}}
  const writer={properties:{write:true},writeValueWithResponse:async(data)=>{command+=new TextDecoder().decode(data);if(!command.endsWith('\r\n'))return;const s=command.trim();command='';if(s==='STATUS')emit('S,SEEN=0,TARE_SEQ=0\r\n');if(s.startsWith('TARE,')){tareCalls++;emit('ACK,'+s+'\r\n');}if(s==='PING')emit('OK,PING\r\n');if(s==='WAKE')emit('OK,WAKE\r\n');}};
  const service={getCharacteristic:async(id)=>{gets++;return id===0xfff1?notify:writer;}};
  const device=new EventTarget();device.name='BT_TEST_SCALE';device.gatt={connected:false,connect:async()=>{connects++;device.gatt.connected=true;return {getPrimaryService:async()=>service,disconnect:()=>device.gatt.disconnect()};},disconnect:()=>{device.gatt.connected=false;device.dispatchEvent(new Event('gattserverdisconnected'));}};
  Object.defineProperty(navigator,'bluetooth',{value:{requestDevice:async()=>device}});
  window.mock={emit:()=>emit(`W,${seq++},120.5,1,20.0,-1,OK\r\n`),drop:()=>device.gatt.disconnect(),stats:()=>({connects,gets,tareCalls})};
 });
 await page.goto('http://localhost:18765');await page.getByRole('button',{name:'连接蓝牙秤',exact:true}).click();await page.waitForFunction(()=>document.getElementById('connection').textContent==='BT_TEST_SCALE');
 await page.evaluate(()=>window.mock.emit());await page.waitForFunction(()=>document.getElementById('weight').textContent==='120.5');
 await page.getByRole('button',{name:'去皮 / 归零净重'}).click();await page.waitForFunction(()=>document.getElementById('message').textContent==='去皮完成');
 await page.selectOption('#units','kg');assert.equal(await page.locator('#weight').textContent(),'0.1205');
 await page.evaluate(()=>window.mock.drop());await page.waitForFunction(()=>document.getElementById('weight').textContent==='—');
 await page.waitForFunction(()=>window.mock.stats().connects===2);await page.waitForFunction(()=>document.getElementById('connection').textContent==='BT_TEST_SCALE');
 const stats=await page.evaluate(()=>window.mock.stats());assert.equal(stats.gets,4);assert.equal(stats.tareCalls,1);
 await page.evaluate(()=>window.mock.emit());await page.waitForFunction(()=>document.getElementById('weight').textContent==='0.1205');
 await page.screenshot({path:'logs/ble-client-preview.png',fullPage:true});
 await page.getByRole('button',{name:'断开连接',exact:true}).click();await page.waitForTimeout(1250);assert.equal((await page.evaluate(()=>window.mock.stats())).connects,2);
 await page.getByRole('button',{name:'体验演示'}).click();await page.waitForTimeout(300);assert.equal(await page.locator('#connection').textContent(),'演示模式');
 await page.setViewportSize({width:390,height:844});await page.screenshot({path:'logs/ble-client-mobile.png',fullPage:true});
 await page.goto('http://localhost:18765/tft-preview.html');await page.getByRole('button',{name:'20g标定向导'}).click();await page.getByRole('button',{name:'SW4长按约2秒'}).click();
 await page.screenshot({path:'logs/tft-preview.png',fullPage:true});assert.deepEqual(errors,[]);
 console.log('PASS: browser UI connection / fragmented notifications / tare ACK / unit / reconnect rediscovery / explicit disconnect / demo / TFT preview');
}finally{await browser?.close();await new Promise(r=>server.close(r));}
