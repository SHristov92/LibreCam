/*
 * Runs bkflash.js against the simulated BootROM in fake_chip.mjs, using the
 * real stock dump of the A13 mini as the chip's flash.
 *   node web-flasher/test/test.mjs
 */
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {SerialIO, BkFlasher, prepareApp, stockApp, crc32, APP_START, APP_END, LC_SETTINGS} from '../bkflash.js';
import {FakeChip, FakePort} from './fake_chip.mjs';

const BACKUP = new Uint8Array(readFileSync(fileURLToPath(new URL(
  '../../backup/a13-mini-bk7252-C8478C61BA0E/flash_full_2MiB.bin', import.meta.url))));
const sleep = ms => new Promise(r => setTimeout(r, ms));

function fakeApp(len) {
  const a = new Uint8Array(len);
  let x = 12345;
  for (let i = 0; i < len; i++) { x = (x * 1103515245 + 12345) >>> 0; a[i] = x >>> 24; }
  a.set([0x0E, 0x00, 0x00, 0xEA]);
  a.fill(0xFF, 0x20000, 0x22000);   // an all-0xFF stretch: erased, not written
  return a;
}

async function setup(opts = {}) {
  const chip = new FakeChip(BACKUP.slice(), opts);
  const port = new FakePort(chip);
  const io = new SerialIO(port);
  await io.open(115200);
  const logs = [];
  const bk = new BkFlasher(io, m => logs.push(m));
  return {chip, port, io, bk, logs};
}

/* Power the camera on a little after the flasher starts waiting, as a user would. */
async function connect(t, opts) {
  setTimeout(() => t.chip.powerOn(), 150);
  return t.bk.connect({linkTimeoutMs: 3000, ...opts});
}

const tests = {
  async 'backup reads the whole flash'() {
    const t = await setup();
    const info = await connect(t);
    assert.equal(info.flashId, 'c84015');
    assert.equal(info.flashSize, 0x200000);
    assert.equal(info.chipId, 0x7252A);
    let last = 0;
    const out = await t.bk.readAll(done => { last = done; });
    assert.equal(last, 0x200000);
    assert.equal(crc32(out), crc32(BACKUP));
    await t.io.close();
  },

  async 'faster baud rate reopens the port and keeps working'() {
    const t = await setup();
    await connect(t, {baud: 921600});
    assert.deepEqual(t.port.opens, [115200, 921600]);
    assert.equal(t.chip.baud, 921600);
    await t.bk.checkBootloader(BACKUP);
    await t.io.close();
  },

  async 'flash an app: only the app region changes'() {
    const t = await setup();
    await connect(t);
    await t.bk.checkBootloader(BACKUP);
    const img = prepareApp(fakeApp(700000));
    let calls = 0;
    await t.bk.writeApp(img, () => calls++);
    assert.equal(calls, img.length / 4096);
    const f = t.chip.flash;
    assert.equal(crc32(f.subarray(APP_START, APP_START + img.length)), crc32(img));
    assert.equal(crc32(f.subarray(0, APP_START)), crc32(BACKUP.subarray(0, APP_START)), 'bootloader untouched');
    assert.equal(crc32(f.subarray(APP_END)), crc32(BACKUP.subarray(APP_END)), 'config/RF untouched');
    assert.equal(t.chip.sr & 0x407C, 0, 'flash unprotected');
    await t.bk.reboot();
    await sleep(20);
    assert.ok(t.chip.rebooted);
    await t.io.close();
  },

  async 'restore stock puts the dump back byte for byte'() {
    const t = await setup();
    await connect(t);
    await t.bk.writeApp(prepareApp(fakeApp(300000)));
    await t.bk.checkBootloader(BACKUP);
    await t.bk.writeApp(stockApp(BACKUP));
    assert.equal(crc32(t.chip.flash), crc32(BACKUP));
    await t.io.close();
  },

  async 'without a backup, the known bootloader is recognised'() {
    const t = await setup();
    await connect(t);
    assert.match(await t.bk.checkKnownBootloader(), /v1\.0\.14/);
    await t.io.close();
  },

  async 'without a backup, an unknown bootloader stops before any write'() {
    const t = await setup();
    t.chip.flash[0x100] ^= 1;
    await connect(t);
    await assert.rejects(t.bk.checkKnownBootloader(), /not one LibreCam knows/);
    assert.ok(!t.chip.log.some(c => c.isLong && (c.code === 0x07 || c.code === 0x0F || c.code === 0x0D)));
    await t.io.close();
  },

  async 'a camera running LibreCam is told apart from stock'() {
    const t = await setup();
    await connect(t);
    assert.equal(await t.bk.runsLibreCam(), false, 'the stock dump');
    t.chip.flash.set([0x56, 0x4B, 0x39, 0x41, 0x10, 0, 0, 0], LC_SETTINGS);
    assert.equal(await t.bk.runsLibreCam(), true, 'settings saved by LibreCam');
    await t.io.close();
  },

  async 'a backup from another camera stops before any write'() {
    const t = await setup();
    await connect(t);
    const other = BACKUP.slice();
    other[0x100] ^= 1;
    await assert.rejects(t.bk.checkBootloader(other), /does not match the backup/);
    assert.ok(!t.chip.log.some(c => c.isLong && (c.code === 0x07 || c.code === 0x0F || c.code === 0x0D)));
    await t.io.close();
  },

  async 'a corrupted read is retried'() {
    const t = await setup();
    await connect(t);
    t.chip.corruptNextRead = 2;
    const d = await t.bk.read4k(0x40000);
    assert.equal(crc32(d), crc32(BACKUP.subarray(0x40000, 0x41000)));
    assert.ok(t.logs.some(l => /retrying/.test(l)));
    await t.io.close();
  },

  async 'unknown flash chip is not written'() {
    const t = await setup({flashId: [0x12, 0x34, 0x15]});
    await connect(t);
    await assert.rejects(t.bk.writeApp(prepareApp(fakeApp(8192))), /Unknown flash chip/);
    assert.equal(crc32(t.chip.flash), crc32(BACKUP));
    await t.io.close();
  },

  async 'flash that stays protected is caught by the first erase'() {
    const t = await setup({flashId: [0xC8, 0x40, 0x15]});
    await connect(t);
    t.bk.unprotect = async () => {};          // as if the SR write silently failed
    await assert.rejects(t.bk.writeApp(prepareApp(fakeApp(8192))), /write-protected/);
    await t.io.close();
  },

  async 'a camera that already booted is caught after a power cycle'() {
    const t = await setup();
    t.chip.powerOn();
    await sleep(100);                         // missed the window: the app runs
    assert.equal(t.chip.state, 'app');
    assert.equal(await t.bk.waitForLink(300), false);
    t.chip.powerOff();
    setTimeout(() => t.chip.powerOn(), 100);
    assert.equal(await t.bk.waitForLink(2000), true);
    await t.io.close();
  },

  async 'cancel stops a running backup'() {
    const t = await setup();
    await connect(t);
    const p = t.bk.readAll(done => { if (done === 0x10000) t.bk.cancel(); });
    await assert.rejects(p, /Cancelled/);
    await t.io.close();
  },

  async 'image checks'() {
    assert.throws(() => prepareApp(new Uint8Array(100)), /not a BK7252 app image/);
    const big = new Uint8Array(APP_END - APP_START + 1);
    big.set([0x0E, 0, 0, 0xEA]);
    assert.throws(() => prepareApp(big), /RF calibration/);
    assert.equal(prepareApp(fakeApp(5000)).length, 8192);
    assert.throws(() => stockApp(new Uint8Array(1000)), /2 MiB/);
    assert.equal(stockApp(BACKUP).length, APP_END - APP_START);
  },
};

let failed = 0;
for (const [name, fn] of Object.entries(tests)) {
  const t0 = Date.now();
  try {
    await fn();
    console.log(`ok   ${name} (${Date.now() - t0} ms)`);
  } catch (e) {
    failed++;
    console.log(`FAIL ${name}\n     ${e.stack}`);
  }
}
process.exit(failed ? 1 : 0);
