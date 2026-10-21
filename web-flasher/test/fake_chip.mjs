/*
 * A simulated BK7252 BootROM behind a Web Serial-shaped port, for testing
 * bkflash.js under Node. It models what the real chip does that the flasher
 * depends on:
 *  - the ROM listens for a link check only for a short window after power-on,
 *    then boots the app (which prints a log and ignores commands);
 *  - bytes only get through when the port and the chip use the same baud rate,
 *    and SetBaudRate answers at the new rate after its delay;
 *  - addresses wrap at the flash size, and writes/erases below the app region
 *    are refused unless the address carries the flash-size offset;
 *  - the status register's block-protect bits make writes and erases no-ops;
 *  - CheckCRC's end address is inclusive and the result is the inverted CRC.
 */
import {crc32} from '../bkflash.js';

const enc = new TextEncoder();

export class FakeChip {
  constructor(flash, {flashId = [0xC8, 0x40, 0x15], sr = 0x007C, romWindowMs = 40} = {}) {
    this.flash = flash;
    this.size = flash.length;
    this.flashId = flashId;
    this.sr = sr;                    // block-protect bits set, like a fresh camera
    this.romWindowMs = romWindowMs;
    this.baud = 115200;
    this.state = 'off';              // off | window | rom | app
    this.rx = [];
    this.port = null;
    this.log = [];                   // commands seen, for assertions
    this.corruptNextRead = 0;        // flip a bit in the next N read responses
  }

  powerOn() {
    this.state = 'window';
    this.baud = 115200;
    this.rx = [];
    this.windowTimer = setTimeout(() => {
      if (this.state !== 'window') return;
      this.state = 'app';
      this._send(enc.encode('\r\nV:BK7252_1.0.12\r\nchip id=7252a device id=24318000\r\n'));
    }, this.romWindowMs);
  }

  powerOff() {
    clearTimeout(this.windowTimer);
    this.state = 'off';
  }

  /* bytes from the host */
  receive(data, baud) {
    if (this.state === 'off' || this.state === 'app') return;
    if (baud !== this.baud) return;  // garbage on the chip side; the ROM drops it
    this.rx.push(...data);
    this._parse();
  }

  _send(bytes) {
    if (this.port) this.port._fromChip(bytes, this.baud);
  }

  _parse() {
    for (;;) {
      let i = 0;
      while (i + 3 <= this.rx.length && !(this.rx[i] === 0x01 && this.rx[i + 1] === 0xE0 && this.rx[i + 2] === 0xFC)) i++;
      if (i) this.rx.splice(0, i);
      if (this.rx.length < 5) return;
      let isLong = false, size, hdr;
      if (this.rx[3] === 0xFF) {
        if (this.rx.length < 8) return;
        if (this.rx[4] !== 0xF4) { this.rx.splice(0, 3); continue; }
        isLong = true;
        size = this.rx[5] | this.rx[6] << 8;
        hdr = 7;
      } else {
        size = this.rx[3];
        hdr = 4;
      }
      if (this.rx.length < hdr + size) return;
      const code = this.rx[hdr];
      const data = Uint8Array.from(this.rx.slice(hdr + 1, hdr + size));
      this.rx.splice(0, hdr + size);
      this._handle(code, data, isLong);
    }
  }

  _short(code, payload) {
    const p = Uint8Array.from(payload);
    const out = new Uint8Array(7 + p.length);
    out.set([0x04, 0x0E, 4 + p.length, 0x01, 0xE0, 0xFC, code]);
    out.set(p, 7);
    setTimeout(() => this._send(out), 1);
  }

  _long(code, payload) {
    const p = Uint8Array.from(payload);
    const n = p.length + 1;
    const out = new Uint8Array(10 + p.length);
    out.set([0x04, 0x0E, 0xFF, 0x01, 0xE0, 0xFC, 0xF4, n & 0xFF, n >> 8, code]);
    out.set(p, 10);
    setTimeout(() => this._send(out), 1);
  }

  _addr(a) {
    return {phys: a % this.size, bypass: a >= this.size};
  }

  _writable(a, len) {
    const {phys, bypass} = this._addr(a);
    if (this.sr & 0x407C) return false;
    if (phys < 0x11000 && !bypass) return false;
    return phys + len <= this.size;
  }

  _handle(code, d, isLong) {
    const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const u32 = o => v.getUint32(o, true);
    const le = n => [n & 0xFF, n >> 8 & 0xFF, n >> 16 & 0xFF, n >>> 24];
    this.log.push({code, isLong});
    if (!isLong) {
      switch (code) {
        case 0x00:
          if (this.state === 'window') { clearTimeout(this.windowTimer); this.state = 'rom'; }
          return this._short(0x01, [0x00]);
        case 0x03:
          return this._short(0x03, [...le(u32(0)), ...le(u32(0) === 0x800000 ? 0x7252A : 0)]);
        case 0x0E:
          if (d[0] === 0xA5) { this.state = 'app'; this.rebooted = true;
            setTimeout(() => this._send(enc.encode('\r\nLibreCam booting\r\n')), 5); }
          return;
        case 0x0F: {
          const baud = u32(0), delay = d[4];
          setTimeout(() => { this.baud = baud; this._short(0x0F, d); }, delay);
          return;
        }
        case 0x10: {
          const s = this._addr(u32(0)).phys, e = this._addr(u32(4)).phys;
          const crc = crc32(this.flash.subarray(s, e + 1));
          return this._short(0x10, le((crc ^ 0xFFFFFFFF) >>> 0));
        }
      }
      return;
    }
    switch (code) {
      case 0x09: {
        const {phys} = this._addr(u32(0));
        const data = this.flash.slice(phys, phys + 4096);
        if (this.corruptNextRead > 0) { this.corruptNextRead--; data[100] ^= 0x10; }
        return this._long(0x09, [0, ...le(u32(0)), ...data]);
      }
      case 0x07: {
        const a = u32(0), {phys} = this._addr(a);
        if (this._writable(a, 4096))
          for (let i = 0; i < 4096; i++) this.flash[phys + i] &= d[4 + i];   // NOR: 1 -> 0 only
        return this._long(0x07, [0, ...le(a)]);
      }
      case 0x0F: {
        const a = u32(1), {phys} = this._addr(a);
        if (d[0] === 0x20 && this._writable(a, 4096)) this.flash.fill(0xFF, phys, phys + 4096);
        return this._long(0x0F, [0, ...d]);
      }
      case 0x0C: {
        const val = d[0] === 0x05 ? this.sr & 0xFF : d[0] === 0x35 ? this.sr >> 8 : 0;
        return this._long(0x0C, [0, d[0], val]);
      }
      case 0x0D: {
        this.sr = d.length === 2 ? d[1] : d[1] | d[2] << 8;
        return this._long(0x0D, [0, ...d]);
      }
      case 0x0E:
        return this._long(0x0E, [0, d[0], ...this.flashId]);
    }
  }
}

/* The host side: a port shaped like Web Serial's SerialPort. */
export class FakePort {
  constructor(chip) {
    this.chip = chip;
    chip.port = this;
    this.baud = 0;
    this.readable = this.writable = null;
    this.opens = [];
  }

  async open({baudRate}) {
    if (this.readable) throw new Error('port already open');
    this.baud = baudRate;
    this.opens.push(baudRate);
    this.readable = new ReadableStream({start: c => { this.ctrl = c; }});
    this.writable = new WritableStream({
      write: chunk => this.chip.receive(Uint8Array.from(chunk), this.baud),
    });
  }

  _fromChip(bytes, baud) {
    if (!this.ctrl) return;                       // closed: lost
    if (baud !== this.baud) bytes = bytes.map(b => b ^ 0x5A);   // wrong rate: garbage
    try { this.ctrl.enqueue(bytes); } catch (e) {}
  }

  async close() {
    if (this.readable.locked || this.writable.locked) throw new Error('streams still locked');
    this.ctrl = null;
    this.readable = this.writable = null;
  }
}
