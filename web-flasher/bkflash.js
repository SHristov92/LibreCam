/*
 * Beken BK72xx UART bootloader client for the browser (Web Serial).
 *
 * The protocol follows bk7231tools (MIT, Kuba Szczodrzyński), which
 * firmware/hal/bk7252/flash.py uses: the same commands, the same address
 * offset that bypasses the bootloader's write protection, the same flash
 * unprotect and the same CRC checks. Only the FULL (BootROM) protocol is
 * supported, as in flash.py.
 *
 * The module has no DOM code; SerialIO takes anything shaped like a Web
 * Serial port, which is how the tests drive it with a simulated chip.
 */

export const APP_START = 0x11000;        // app region (physical), after the bootloader
export const APP_END = 0x1E0000;         // RF calibration and config start here
export const BOOT_LEN = 0x11000;
export const SECTOR = 0x1000;

const CMD_PREAMBLE = [0x01, 0xE0, 0xFC];
const RESP_PREAMBLE = Uint8Array.of(0x04, 0x0E);
const RESP_DATA = Uint8Array.of(0x01, 0xE0, 0xFC);
const RESP_LONG = Uint8Array.of(0xF4);
const CRC32_FF_4K = 0xF154670A;          // CRC-32 of 4096 bytes of 0xFF
const CRC_BYTES_PER_S = 400000;          // how fast the chip computes CRCs

/* ---- CRC-32 (zlib) ---- */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(data, crc = 0) {
  crc = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) crc = CRC_TABLE[(crc ^ data[i]) & 0xFF] ^ (crc >>> 8);
  return ~crc >>> 0;
}

export const hex = (n, w = 8) => '0x' + (n >>> 0).toString(16).padStart(w, '0');
const sleep = ms => new Promise(r => setTimeout(r, ms));

function indexOf(buf, len, pat, from = 0) {
  outer: for (let i = from; i + pat.length <= len; i++) {
    for (let j = 0; j < pat.length; j++) if (buf[i + j] !== pat[j]) continue outer;
    return i;
  }
  return -1;
}

/* ---- Serial port with a receive buffer ---- */

export class TimeoutError extends Error {}

export class SerialIO {
  constructor(port) {
    this.port = port;
    this.buf = new Uint8Array(1 << 16);
    this.len = 0;
    this.waiter = null;
    this.baud = 0;
    this.onData = null;          // optional tap for the boot log
  }

  async open(baud) {
    await this.port.open({baudRate: baud, bufferSize: 1 << 16});
    this.baud = baud;
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.pump = this._pump(this.reader);
  }

  async _pump(reader) {
    try {
      for (;;) {
        const {value, done} = await reader.read();
        if (done) break;
        if (!value || !value.length) continue;
        if (this.onData) this.onData(value);
        this._push(value);
      }
    } catch (e) {
      /* cancelled or the device went away */
    } finally {
      reader.releaseLock();
    }
  }

  _push(data) {
    if (this.len + data.length > this.buf.length) {
      const n = new Uint8Array(Math.max(this.buf.length * 2, this.len + data.length));
      n.set(this.buf.subarray(0, this.len));
      this.buf = n;
    }
    this.buf.set(data, this.len);
    this.len += data.length;
    if (this.waiter) { const w = this.waiter; this.waiter = null; w(); }
  }

  async close() {
    if (!this.reader) return;
    try { await this.reader.cancel(); } catch (e) {}
    await this.pump;
    this.writer.releaseLock();
    this.reader = this.writer = null;
    await this.port.close();
  }

  /* Web Serial can't change the rate of an open port: reopen it. */
  async reopen(baud) {
    await this.close();
    await this.open(baud);
  }

  async write(data) {
    await this.writer.write(data);
  }

  _take(n) {
    const out = this.buf.slice(0, n);
    this.buf.copyWithin(0, n, this.len);
    this.len -= n;
    return out;
  }

  _wait(deadline) {
    const left = deadline - Date.now();
    if (left <= 0) return Promise.resolve(false);
    return new Promise(res => {
      const t = setTimeout(() => { this.waiter = null; res(false); }, left);
      this.waiter = () => { clearTimeout(t); res(true); };
    });
  }

  async read(n, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (this.len < n) {
      if (!(await this._wait(deadline))) throw new TimeoutError(`timed out waiting for ${n} bytes`);
    }
    return this._take(n);
  }

  /* Returns everything up to and including `pat`. */
  async readUntil(pat, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const i = indexOf(this.buf, this.len, pat);
      if (i >= 0) return this._take(i + pat.length);
      if (!(await this._wait(deadline))) throw new TimeoutError('timed out waiting for a response');
    }
  }

  drain() {
    this.len = 0;
  }
}

/* ---- Flash chips: status register size by JEDEC id (from bk7231tools) ---- */

const FLASH_SR_SIZE = {
  '0b4014': 2, '0b4015': 2, '0b4016': 2, '0b4017': 2, '0b6017': 2, '0e4016': 2,
  '1c3113': 1, '1c4116': 1, '1c7015': 1, '1c7016': 1, '204016': 2, '514013': 1,
  '514014': 1, '5e4014': 1, '854215': 1, '852015': 2, '856013': 2, '856014': 2,
  '856016': 2, '856017': 2, 'c22314': 2, 'c22315': 2, 'c84013': 1, 'c84014': 2,
  'c84015': 2, 'c84016': 1, 'c86515': 2, 'c86516': 2, 'c86517': 2, 'cd6014': 2,
  'e04013': 1, 'e04014': 1, 'eb6015': 2, 'ef4016': 2, 'ef4018': 2,
};

/* ---- Bootloader client ---- */

export class BkFlasher {
  constructor(io, log = () => {}) {
    this.io = io;
    this.log = log;
    this.timeout = 1000;         // per command, ms
    this.flashSize = 0;          // set once the flash id is known
    this.flashId = null;
    this.eraseChecked = false;
    this.cancelled = false;
  }

  cancel() { this.cancelled = true; }
  _checkCancel() { if (this.cancelled) throw new Error('Cancelled'); }

  /* Addresses past the flash size wrap around to the same cell but skip
   * the bootloader's protection check (bk7231tools' boot_protection_bypass). */
  fixAddr(a) { return this.flashSize ? a + this.flashSize : a; }

  static encode(code, data, isLong) {
    const size = data.length + 1;
    const out = [...CMD_PREAMBLE];
    if (size >= 0xFF || isLong) out.push(0xFF, 0xF4, size & 0xFF, size >> 8);
    else out.push(size);
    out.push(code);
    const b = new Uint8Array(out.length + data.length);
    b.set(out);
    b.set(data, out.length);
    return b;
  }

  /* Sends a command and returns the response payload (or null when the
   * command has none). respCode defaults to the command code. */
  async command(code, data, {isLong = false, resp = true, respCode = code, timeout = this.timeout, afterSend} = {}) {
    this._checkCancel();
    await this.io.write(BkFlasher.encode(code, data, isLong));
    if (afterSend) await afterSend();
    if (!resp) return null;
    const io = this.io;
    for (;;) {
      const pre = await io.readUntil(RESP_PREAMBLE, timeout);
      if (pre.length < 2) continue;
      let size = (await io.read(1, timeout))[0];
      if (isLong !== (size === 0xFF)) continue;
      await io.readUntil(RESP_DATA, timeout);
      let rcode;
      if (isLong) {
        await io.readUntil(RESP_LONG, timeout);
        const h = await io.read(3, timeout);
        size = (h[0] | h[1] << 8) - 1;
        rcode = h[2];
      } else {
        rcode = (await io.read(1, timeout))[0];
        size -= 4;
      }
      if (rcode !== respCode) continue;
      return io.read(size, timeout);
    }
  }

  /* Sends the link check every few ms until the bootloader answers, which
   * catches it while the camera is power-cycled. */
  async waitForLink(timeoutMs, onTick) {
    const deadline = Date.now() + timeoutMs;
    this.io.drain();
    while (Date.now() < deadline) {
      this._checkCancel();
      try {
        /* an empty command 0x00 is exactly the link check frame */
        const r = await this.command(0x00, new Uint8Array(0), {respCode: 0x01, timeout: 10});
        if (r[0] === 0) { await sleep(20); this.io.drain(); return true; }
      } catch (e) {
        if (!(e instanceof TimeoutError)) throw e;
      }
      if (onTick) onTick(deadline - Date.now());
    }
    return false;
  }

  async setBaudrate(baud) {
    const delay = 200;           // the chip switches after this; we reopen the port meanwhile
    const d = new Uint8Array(5);
    new DataView(d.buffer).setUint32(0, baud, true);
    d[4] = delay;
    const r = await this.command(0x0F, d, {
      timeout: 2000,
      afterSend: async () => { await sleep(30); await this.io.reopen(baud); },
    });
    for (let i = 0; i < 5; i++) if (r[i] !== d[i]) throw new Error('Baud rate change was not confirmed');
  }

  async readReg(addr) {
    const d = new Uint8Array(4);
    new DataView(d.buffer).setUint32(0, addr, true);
    const r = await this.command(0x03, d);
    return new DataView(r.buffer, r.byteOffset).getUint32(4, true);
  }

  async reboot() {
    await this.command(0x0E, Uint8Array.of(0xA5), {resp: false});
  }

  /* CRC-32 of [start, end) as the chip computes it (FULL protocol: its end
   * is inclusive, hence end - 1). */
  async crcRange(start, end) {
    const d = new Uint8Array(8), v = new DataView(d.buffer);
    v.setUint32(0, this.fixAddr(start), true);
    v.setUint32(4, this.fixAddr(end) - 1, true);
    const timeout = Math.max(this.timeout, Math.ceil((end - start) / CRC_BYTES_PER_S * 1000) + 500);
    const r = await this.command(0x10, d, {timeout});
    return (new DataView(r.buffer, r.byteOffset).getUint32(0, true) ^ 0xFFFFFFFF) >>> 0;
  }

  async checkCrc(start, data) {
    const chip = await this.crcRange(start, start + data.length), calc = crc32(data);
    if (chip !== calc) throw new Error(`CRC mismatch at ${hex(start)}: chip ${hex(chip)}, expected ${hex(calc)}`);
  }

  async flashReg8(cmd) {
    const r = await this.command(0x0C, Uint8Array.of(cmd), {isLong: true});
    return r[2];
  }

  async flashGetId() {
    const d = Uint8Array.of(0x9F, 0, 0, 0);
    const r = await this.command(0x0E, d, {isLong: true});
    this.flashId = r.slice(2, 5);
    return this.flashId;
  }

  async readSR(size) {
    let sr = await this.flashReg8(0x05);
    if (size === 2) sr |= (await this.flashReg8(0x35)) << 8;
    return sr;
  }

  async writeSR(sr, size, mask) {
    const d = size === 1 ? Uint8Array.of(0x01, sr & 0xFF) : Uint8Array.of(0x01, sr & 0xFF, sr >> 8);
    await this.command(0x0D, d, {isLong: true});
    const got = await this.readSR(size);
    if ((got & mask) !== (sr & mask)) throw new Error(`Flash status register write failed: wrote ${hex(sr, 4)}, read ${hex(got, 4)}`);
  }

  /* Clears the flash block-protect bits. */
  async unprotect(mask = 0b0100000001111100) {
    const id = Array.from(this.flashId, b => b.toString(16).padStart(2, '0')).join('');
    const size = FLASH_SR_SIZE[id];
    if (!size) throw new Error(`Unknown flash chip ${id}; not writing`);
    const sr = await this.readSR(size);
    if (sr & mask) await this.writeSR(sr & ~mask, size, mask);
  }

  async read4k(start, check = true) {
    let lastErr;
    for (let attempt = 0; attempt <= 20; attempt++) {
      this._checkCancel();
      try {
        const d = new Uint8Array(4);
        new DataView(d.buffer).setUint32(0, this.fixAddr(start), true);
        const r = await this.command(0x09, d, {isLong: true, timeout: 3000});
        if (r[0] !== 0) throw new Error(`read status ${r[0]}`);
        const data = r.slice(5);
        if (data.length !== SECTOR) throw new Error(`short read (${data.length} bytes)`);
        if (check) await this.checkCrc(start, data);
        return data;
      } catch (e) {
        if (this.cancelled) throw e;
        lastErr = e;
        this.log(`Read at ${hex(start)} failed (${e.message}), retrying`);
        this.io.drain();
      }
    }
    throw lastErr;
  }

  async erase4k(start) {
    const run = async () => {
      const d = new Uint8Array(5);
      d[0] = 0x20;
      new DataView(d.buffer).setUint32(1, this.fixAddr(start), true);
      await this.command(0x0F, d, {isLong: true, timeout: 3000});
    };
    if (this.eraseChecked) return run();
    /* The first erase is verified: a protected flash ignores it silently. */
    if ((await this.crcRange(start, start + SECTOR)) === CRC32_FF_4K) return;
    await run();
    if ((await this.crcRange(start, start + SECTOR)) !== CRC32_FF_4K)
      throw new Error(`Erase at ${hex(start)} had no effect: the flash is write-protected`);
    this.eraseChecked = true;
  }

  async write4k(start, data) {
    let lastErr;
    for (let attempt = 0; attempt <= 3; attempt++) {
      this._checkCancel();
      try {
        const d = new Uint8Array(4 + SECTOR);
        new DataView(d.buffer).setUint32(0, this.fixAddr(start), true);
        d.set(data, 4);
        const r = await this.command(0x07, d, {isLong: true, timeout: 3000});
        if (r[0] !== 0) throw new Error(`write status ${r[0]}`);
        await this.checkCrc(start, data);
        return;
      } catch (e) {
        if (this.cancelled) throw e;
        lastErr = e;
        this.log(`Write at ${hex(start)} failed (${e.message}), retrying`);
        this.io.drain();
        await this.erase4k(start);
      }
    }
    throw lastErr;
  }

  /* Links, optionally speeds up, and identifies the chip. Refuses anything
   * but the FULL protocol, like flash.py. */
  async connect({baud = 115200, linkTimeoutMs = 120000, onWaiting} = {}) {
    if (!(await this.waitForLink(linkTimeoutMs, onWaiting)))
      throw new Error('The bootloader did not answer. Check the wiring (adapter TX to RX1, RX to TX1, GND) and power-cycle the camera while this waits.');
    this.log('Bootloader answered');
    if (baud !== this.io.baud) {
      this.log(`Switching to ${baud} baud`);
      await this.setBaudrate(baud);
    }
    /* Protocol check, as bk7231tools does for unknown bootloaders: the BootROM's
     * CRC end is inclusive. Read 4k without the protection offset first. */
    const sample = await this.read4k(APP_START, false);
    const d = new Uint8Array(8), v = new DataView(d.buffer);
    v.setUint32(0, APP_START, true);
    v.setUint32(4, APP_START + 256, true);
    const r = await this.command(0x10, d);
    const crc = (new DataView(r.buffer, r.byteOffset).getUint32(0, true) ^ 0xFFFFFFFF) >>> 0;
    if (crc !== crc32(sample.subarray(0, 257)))
      throw new Error('This bootloader does not speak the full BootROM protocol; use flash.py instead.');
    let chipId = null;
    try { chipId = await this.readReg(0x800000); } catch (e) { this.io.drain(); }
    const id = await this.flashGetId();
    this.flashSize = 1 << id[2];
    const info = {chipId, flashId: Array.from(id, b => b.toString(16).padStart(2, '0')).join(''), flashSize: this.flashSize};
    this.log(`Chip id ${chipId === null ? 'unknown' : hex(chipId, 5)}, flash ${info.flashId}, ${this.flashSize / 1024} KiB`);
    return info;
  }

  async readAll(onProgress) {
    const out = new Uint8Array(this.flashSize);
    for (let a = 0; a < this.flashSize; a += SECTOR) {
      out.set(await this.read4k(a), a);
      if (onProgress) onProgress(a + SECTOR, this.flashSize);
    }
    return out;
  }

  /* The chip must run the bootloader the backup was taken from. */
  async checkBootloader(backup) {
    const chip = await this.crcRange(0, BOOT_LEN), want = crc32(backup.subarray(0, BOOT_LEN));
    if (chip !== want)
      throw new Error(`The bootloader on this camera (${hex(chip)}) does not match the backup (${hex(want)}). Wrong camera or wrong backup file; nothing was written.`);
  }

  /* Writes payload at APP_START, sector by sector, each one CRC-checked,
   * then checks the whole range once more. */
  async writeApp(payload, onProgress) {
    await this.unprotect();
    for (let off = 0; off < payload.length; off += SECTOR) {
      const block = payload.subarray(off, off + SECTOR);
      await this.erase4k(APP_START + off);
      if (block.some(b => b !== 0xFF)) await this.write4k(APP_START + off, block);
      if (onProgress) onProgress(off + SECTOR, payload.length);
    }
    const chip = await this.crcRange(APP_START, APP_START + payload.length), want = crc32(payload);
    if (chip !== want) throw new Error(`Verify failed: chip ${hex(chip)}, image ${hex(want)}. Run it again, or restore the stock firmware.`);
  }
}

/* ---- Image checks shared by the page and the tests ---- */

/* Checks a LibreCam image (lc_app.bin) and pads it to whole sectors. */
export function prepareApp(image) {
  if (image.length < 4 || image[0] !== 0x0E || image[1] !== 0x00 || image[2] !== 0x00 || image[3] !== 0xEA)
    throw new Error('This file is not a BK7252 app image (it should start with the app vector table).');
  if (APP_START + image.length > APP_END)
    throw new Error('This image would overwrite the RF calibration and config area; not writing it.');
  const padded = new Uint8Array(Math.ceil(image.length / SECTOR) * SECTOR).fill(0xFF);
  padded.set(image);
  return padded;
}

/* The stock app region of a full 2 MiB backup. */
export function stockApp(backup) {
  if (backup.length !== 0x200000) throw new Error('A stock backup is the full 2 MiB flash dump.');
  return backup.slice(APP_START, APP_END);
}
