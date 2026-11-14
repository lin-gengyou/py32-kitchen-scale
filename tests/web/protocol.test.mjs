import {test} from 'node:test';
import assert from 'node:assert/strict';
import {parseFrame, LineParser, CommandChannel} from '../../web/protocol.mjs';
const enc = new TextEncoder();
test('任意分包、粘包、CRLF、错误后恢复',()=>{
  const text='W,4294967295,-0.5,1,20.0,-1,OK\r\nACK,TARE,3\r\nW,0,,0,,-1,ERR\r\n';
  for(let cut=0;cut<=text.length;cut++) {
    const received=[];const parser=new LineParser(f=>received.push(f));
    parser.push(enc.encode(text.slice(0,cut)));parser.push(enc.encode(text.slice(cut)));
    assert.equal(received.length,3);assert.equal(received[0].net,-.5);assert.equal(received[2].net,null);
  }
  const received=[];const p=new LineParser(f=>received.push(f));
  p.push(enc.encode('X'.repeat(500)+'\r\nW,1,12.0,1,0.0,-1,OK\r\n'));
  assert.equal(received.length,1);assert.equal(received[0].net,12);
  assert.equal(parseFrame('W,1,NaN,1,0.0,-1,OK'),null);
  assert.equal(parseFrame('W,1,1.0,1,0.0,101,OK'),null);
  assert.equal(parseFrame('W,1,1.0,1,0.0,-1,ERR'),null);
  assert.deepEqual(parseFrame('S,SEEN=1,TARE_SEQ=4294967295'),{type:'state',seen:true,seq:4294967295});
});
test('丢失ACK时重复原序号，忽略别的应答',async()=>{
  const writes=[];let channel;
  channel=new CommandChannel(async text=>{writes.push(text);
    channel.accept({type:'tare',seq:99});
    if(writes.length===2)channel.accept({type:'tare',seq:7,ok:true});
  },{timeout:5,retries:2});
  const result=await channel.request('TARE,7',f=>f.type==='tare'&&f.seq===7);
  assert.equal(result.ok,true);assert.deepEqual(writes,['TARE,7\r\n','TARE,7\r\n']);
});
test('错误应答不重发、单一在途、断线清理',async()=>{
  let channel;let writes=0;
  channel=new CommandChannel(async()=>{writes++;channel.accept({type:'tare',seq:1,ok:false,reason:'UNSTABLE'});});
  assert.equal((await channel.request('TARE,1',f=>f.seq===1)).reason,'UNSTABLE');assert.equal(writes,1);
  channel=new CommandChannel(async()=>{}, {timeout:100});
  const pending=channel.request('PING',()=>false);
  await assert.rejects(channel.request('WAKE',()=>false),/上一条/);
  channel.abort('已断开');await assert.rejects(pending,/已断开/);assert.equal(channel.pending,null);
});
test('重试次数有界、写入异常及时结束',async()=>{
  let n=0;const c=new CommandChannel(async()=>{n++;},{timeout:3,retries:2});
  await assert.rejects(c.request('PING',()=>false),/超时/);assert.equal(n,3);
  const failed=new CommandChannel(async()=>{throw new Error('写入断开');});
  await assert.rejects(failed.request('PING',()=>false),/写入断开/);
});

test('底层写入没有返回时也有超时',async()=>{
  const c=new CommandChannel(()=>new Promise(()=>{}),{timeout:5});
  await assert.rejects(c.request('PING',()=>false),/写入超时/);
});
