// 与固件共用的ASCII协议。独立于DOM，浏览器与Node测试使用同一解析器。
export const statuses = {
  WAIT: '等待采样', UNCAL: '请先在秤上标定', OK: '称重', RANGE: '超出量程',
  OL: '超载，请移走物品', ERR: '称重采集故障', LOWBAT: '电池电压低',
  POWERERR: '电源采样故障', CALIBRATING: '秤上正在标定', SLEEP: '秤已待机'
};
const uint32 = s => /^(0|[1-9]\d{0,9})$/.test(s) && Number(s) <= 0xffffffff;
const grams = s => /^-?\d+\.\d$/.test(s) && Math.abs(Number(s)) <= 20000;
export function parseFrame(line) {
  const p = line.split(',');
  if (p[0] === 'W' && p.length === 7 && uint32(p[1]) && /^[01]$/.test(p[3]) &&
      /^(-1|\d{1,3})$/.test(p[5]) && Number(p[5]) <= 100 && Object.hasOwn(statuses, p[6])) {
    const good = p[6] === 'OK';
    if (good ? !grams(p[2]) || !grams(p[4]) : p[2] !== '' || p[4] !== '' || p[3] !== '0') return null;
    return {type: 'weight', seq: Number(p[1]), net: good ? Number(p[2]) : null,
      stable: p[3] === '1', tare: good ? Number(p[4]) : null, battery: Number(p[5]), status: p[6]};
  }
  if (p[0] === 'S' && p.length === 3 && /^SEEN=[01]$/.test(p[1]) && p[2].startsWith('TARE_SEQ=') && uint32(p[2].slice(9)))
    return {type: 'state', seen: p[1] === 'SEEN=1', seq: Number(p[2].slice(9))};
  if ((p[0] === 'ACK' || p[0] === 'ERR') && p[1] === 'TARE' && uint32(p[2] || '') &&
      (p[0] === 'ACK' ? p.length === 3 : p.length === 4 && /^[A-Z_]+$/.test(p[3])))
    return {type: 'tare', ok: p[0] === 'ACK', seq: Number(p[2]), reason: p[3] || ''};
  if (p.length === 2 && p[0] === 'OK' && ['PING', 'WAKE'].includes(p[1]))
    return {type: 'reply', command: p[1]};
  return null;
}

// 通知可以在任意字节分割；超长/非法帧一直丢弃到换行，下一帧重新同步。
export class LineParser {
  constructor(onFrame) { this.onFrame = onFrame; this.reset(); }
  reset() { this.text = ''; this.bad = false; }
  push(bytes) {
    for (const byte of bytes) {
      if (byte === 10 || byte === 13) {
        if (this.text && !this.bad) { const frame = parseFrame(this.text); if (frame) this.onFrame(frame); }
        this.reset();
      } else if (byte < 32 || byte > 126 || this.text.length >= 159) this.bad = true;
      else if (!this.bad) this.text += String.fromCharCode(byte);
    }
  }
}

// 同一连接上只允许一条在途指令。超时重复发送同一TARE序号，收到ERR即结束本次操作。
export class CommandChannel {
  constructor(write, {timeout = 1800, retries = 2} = {}) {
    this.write = write; this.timeout = timeout; this.retries = retries; this.pending = null;
  }
  accept(frame) {
    const p = this.pending;
    if (!p || !p.match(frame)) return;
    clearTimeout(p.timer); this.pending = null; p.resolve(frame);
  }
  abort(message = '蓝牙连接已断开') {
    if (!this.pending) return;
    clearTimeout(this.pending.timer); this.pending.reject(new Error(message)); this.pending = null;
  }
  request(command, match) {
    if (this.pending) return Promise.reject(new Error('上一条操作正在确认'));
    return new Promise((resolve, reject) => {
      const p = {command, match, resolve, reject, attempts: 0}; this.pending = p;
      const send = async () => {
        if (this.pending !== p) return;
        if (p.attempts++ > this.retries) { this.abort('操作应答超时，请查看秤上显示'); return; }
        p.timer = setTimeout(() => { if (this.pending === p) this.abort('蓝牙写入超时'); }, this.timeout);
        try { await this.write(command + '\r\n'); }
        catch (e) { if (this.pending === p) this.abort(e.message); return; }
        if (this.pending === p) { clearTimeout(p.timer); p.timer = setTimeout(send, this.timeout); }
      };
      send();
    });
  }
}
