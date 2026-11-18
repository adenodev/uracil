/*
 * Iris × Uracil wasm smoke test (node).
 *
 *   node smoke.js [path/to/uracil.js]
 *
 * Boots the kernel module, then drives the shell through the same
 * ccall-based interface the website uses (ccall is asyncify-aware: it
 * returns a promise exactly when the call suspended, e.g. during fetch).
 * Run after `bash uracil/build_wasm.sh`.
 */

const fs = require('fs');
const path = require('path');

const modulePath = process.argv[2] ||
  path.join(__dirname, '..', '..', '..', 'adeno-site', 'app', 'public', 'uracil', 'uracil.js');

/* The emscripten loader is a classic-script/CommonJS bundle, but it lives
 * under a "type": "module" package — evaluate it as CJS so module.exports
 * is honoured. (Browsers load it via <script>, which is unaffected.) */
const source = fs.readFileSync(modulePath, 'utf8');
const sandbox = { exports: {} };
new Function('module', 'exports', 'require', '__dirname', '__filename', source)(
  sandbox, sandbox.exports, require, path.dirname(modulePath), modulePath
);
const UracilModule = sandbox.exports.default || sandbox.exports;

UracilModule().then(async (m) => {
  const exec = (line) => m.ccall('iris_wasm_exec', 'string', ['string'], [line]);
  const screen = () => m.ccall('iris_wasm_screen', 'string', [], []);
  const key = (k) => m.ccall('iris_wasm_key', 'string', ['number'], [k]);

  const run = async (line) => {
    console.log(`\niris❯ ${line}`);
    const out = await exec(line);
    console.log(out);
    return out;
  };

  console.log('--- boot ---');
  console.log(await m.ccall('iris_wasm_boot', 'string', [], []));
  m.ccall('iris_wasm_set_size', null, ['number', 'number'], [72, 16]);

  await run('help');
  await run('ls');
  await run('perm');
  await run('perm grant fs.read');
  await run('perm grant fs.write');
  await run('write note.txt hello from iris');
  await run('ls');
  await run('cat note.txt');
  await run('rm note.txt');
  await run('ls');
  await run('net get https://example.com/');
  await run('perm grant net.client');
  await run('net get https://example.com/');
  await run('perm grant clock.read');
  await run('date');
  await run('uname');
  await run('ps');
  await run('campaign');

  await run('tui monitor');
  const monitor = JSON.parse(await screen());
  console.log('monitor frame:', monitor.app, '·', monitor.lines.length, 'lines');

  /* ── campaign: stage / hold / complete the Kairos-shaped bundle ── */
  console.log('\n--- campaign ---');
  await run('campaign stage');
  const manifestPath = '/urashell/campaigns/iris-demo/artifacts/kairos-enclave/manifest.json';
  console.log('staged manifest on disk:', m.FS.stat(manifestPath).size, 'bytes');
  const stateText = m.FS.readFile('/urashell/campaigns/iris-demo/kernel.state', { encoding: 'utf8' });
  console.log('state file phase:', /^phase (.*)$/m.exec(stateText)[1]);
  await run('campaign load');
  await run('campaign complete');
  let cleaned = false;
  try { m.FS.stat(manifestPath); } catch { cleaned = true; }
  console.log('artifact cleaned after complete:', cleaned);
  await run('campaign destroy');
  let destroyed = false;
  try { m.FS.stat('/urashell/campaigns/iris-demo/kernel.state'); } catch { destroyed = true; }
  console.log('campaign state removed after destroy:', destroyed);

  /* ── doom: the .ura service, run by the kernel (offline: pre-staged) ── */
  console.log('\n--- doom ---');
  await run('perm revoke net.client');
  const refuseNet = await run('tui doom');
  console.log('refused without net.client:', refuseNet.includes('missing permissions: net.client'));
  await run('perm grant net.client');

  /* pre-stage the .ura the way a previous browser session leaves it, so
   * the node run exercises the cached path instead of fetching */
  const pub = path.join(__dirname, '..', '..', '..', 'adeno-site', 'app', 'public', 'doom');
  m.FS.mkdirTree('/urashell/campaigns/iris-demo/artifacts/doom');
  m.FS.writeFile('/urashell/campaigns/iris-demo/artifacts/doom/doom-engine-1.9.0.wasm32.ura',
    new Uint8Array(fs.readFileSync(path.join(pub, 'doom-engine-1.9.0.wasm32.ura'))));

  await run('perm revoke fs.read');
  const refuseFs = await run('tui doom');
  console.log('refused without fs.read (meta gate):', refuseFs.includes('missing permissions:') && refuseFs.includes('fs.read'));
  await run('perm grant fs.read');

  const openOut = await run('tui doom');
  console.log('ura loaded:', openOut.includes('ura: ai.adeno.doom-engine 1.9.0'));
  console.log('meta functions:', openOut.includes('fn doom_start') && openOut.includes('requires: fs.read'));
  console.log('spawned:', openOut.includes('running on slot'));
  const frame1 = JSON.parse(await screen());
  console.log('doom app opened:', frame1.app === 'doom', '· lines:', frame1.lines.length);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  for (let i = 0; i < 60; i++) {
    await m.ccall('iris_wasm_tick', null, [], []);
    await sleep(20);
  }
  const frame2 = JSON.parse(await screen());
  const frameText = frame2.lines.join('\n');
  console.log('after ticks · lines:', frame2.lines.length, '· ansi:', frameText.includes('38;2;'), '· half blocks:', frameText.includes('\u2580'));
  m.ccall('iris_wasm_doom_key', null, ['number', 'number'], [0xad, 1]);
  m.ccall('iris_wasm_doom_key', null, ['number', 'number'], [0xad, 0]);
  await m.ccall('iris_wasm_tick', null, [], []);
  m.ccall('iris_doom_closed', null, [], []);
  console.log('closed:', JSON.parse(await screen()).active === false);

  console.log('\n--- smoke done ---');
}).catch((err) => {
  console.error('smoke failed:', err);
  process.exit(1);
});
