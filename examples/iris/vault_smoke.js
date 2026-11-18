/*
 * Kairos vault × Uracil kernel smoke (node).
 *
 *   node vault_smoke.mjs
 *
 * Boots the kernel, opens the Kairos enclave as a `.ura` service through the
 * generic service host, and drives the full vault flow — all in-kernel:
 * grants → service_open → initialize/unlock → saveEvent/getEvents/status →
 * per-function refusal → close/reopen (fresh instance, same store) → unlock
 * → events still decrypt. Run after `bash uracil/build_wasm.sh` and
 * `kairos-enclave/build_uracil_bundle.sh`.
 */

const fs = require('fs');
const path = require('path');

const modulePath = process.argv[2] ||
  path.join(__dirname, '..', '..', '..', 'adeno-site', 'app', 'public', 'uracil', 'uracil.js');
const uraPath = process.argv[3] ||
  path.join(__dirname, '..', '..', '..', 'kairos-enclave', 'dist', 'kairos-enclave-0.3.1.wasm32.ura');

const source = fs.readFileSync(modulePath, 'utf8');
const sandbox = { exports: {} };
new Function('module', 'exports', 'require', '__dirname', '__filename', source)(
  sandbox, sandbox.exports, require, path.dirname(modulePath), modulePath
);
const UracilModule = sandbox.exports.default || sandbox.exports;

let failures = 0;
function check(label, condition) {
  console.log(`${condition ? 'ok  ' : 'FAIL'} ${label}`);
  if (!condition) failures++;
}

function jwt(payload) {
  const b64 = (value) => Buffer.from(JSON.stringify(value)).toString('base64url');
  return `${b64({ alg: 'HS256', typ: 'JWT' })}.${b64(payload)}.sig`;
}

UracilModule().then(async (m) => {
  const ccall = m.ccall.bind(m);
  const grant = (names) => ccall('iris_wasm_grant', 'string', ['string'], [names]);
  const revoke = (names) => ccall('iris_wasm_revoke', 'string', ['string'], [names]);
  const open = (name, url) => ccall('iris_wasm_service_open', 'string', ['string', 'string'], [name, url]);
  const call = async (name, fn, args = []) => {
    const raw = await ccall('iris_wasm_service_call', 'string', ['string', 'string', 'string'],
      [name, fn, JSON.stringify(args)]);
    try { return JSON.parse(raw); } catch { return { raw }; }
  };

  console.log('--- boot ---');
  await ccall('iris_wasm_boot', 'string', [], []);

  console.log('\n--- grants ---');
  grant('fs.read fs.write clock.read net.client');
  console.log('perms:', (await ccall('iris_wasm_perms', 'string', [], [])).trim().split('\n')[0]);

  console.log('\n--- stage + open the service ---');
  m.FS.mkdirTree('/urashell/services/vault');
  m.FS.writeFile('/urashell/services/vault/kairos-enclave-0.3.0.wasm32.ura',
    new Uint8Array(fs.readFileSync(uraPath)));

  const opened = JSON.parse(open('vault', '/wasm/kairos-enclave-0.3.0.wasm32.ura'));
  check('service_open', opened.ok === true && opened.name === 'ai.adeno.kairos-enclave');
  console.log('   functions:', opened.functions.map((f) => `${f.name}(${f.requires})`).join(' '));

  console.log('\n--- vault flow ---');
  let result = await call('vault', 'initialize', [jwt({ jti: 'smoke-1', sub: 'uid-smoke' }), 'uid-smoke']);
  check('initialize', result.ok === true);

  result = await call('vault', 'status');
  check('locked after initialize', result.vault === false && result.unlocked === false);

  result = await call('vault', 'unlock', ['smoke-code-42']);
  check('unlock creates vault', result.ok === true && typeof result.fingerprint === 'string');

  result = await call('vault', 'saveEvent', [JSON.stringify({
    title: 'Ship the vault', date: '2026-07-15', time: '10:00', duration: 60,
    location: 'Hennepin', event_type: 'milestone',
  })]);
  check('saveEvent', result.ok === true && typeof result.event_id === 'number');
  const eventId = result.event_id;

  result = await call('vault', 'getEvents');
  check('getEvents round-trip', Array.isArray(result) && result.length === 1 &&
    result[0].event.title === 'Ship the vault');

  result = await call('vault', 'status');
  check('status reports sealed state', result.unlocked === true && result.event_count === 1);

  console.log('\n--- per-function gate (from the .ura meta) ---');
  revoke('fs.write');
  result = await call('vault', 'saveEvent', [JSON.stringify({ title: 'blocked' })]);
  check('saveEvent refused without fs.write', result.ok === false && result.error === 'permission' &&
    String(result.missing).includes('fs.write'));
  console.log('   refusal:', result.message);
  grant('fs.write');
  result = await call('vault', 'saveEvent', [JSON.stringify({ title: 'allowed again', event_type: 'note' })]);
  check('saveEvent works after regrant', result.ok === true);

  console.log('\n--- close / reopen (fresh instance, same store) ---');
  ccall('iris_wasm_service_close', null, ['string'], ['vault']);
  const reopened = JSON.parse(open('vault', '/wasm/kairos-enclave-0.3.0.wasm32.ura'));
  check('reopen', reopened.ok === true);

  result = await call('vault', 'initialize', [jwt({ jti: 'smoke-2' }), 'uid-smoke']);
  check('initialize after reopen', result.ok === true);
  result = await call('vault', 'status');
  check('vault persists, locked', result.vault === true && result.unlocked === false && result.event_count === 2);

  result = await call('vault', 'unlock', ['wrong']);
  check('wrong code rejected in-kernel', result.ok === false);
  result = await call('vault', 'unlock', ['smoke-code-42']);
  check('reopen + unlock', result.ok === true);

  result = await call('vault', 'getEvents');
  check('events decrypt after reopen', Array.isArray(result) && result.length === 2 &&
    result.some((e) => e.event.title === 'Ship the vault'));

  result = await call('vault', 'deleteEvent', [JSON.stringify({ event_id: eventId })]);
  check('deleteEvent', result.ok === true);
  result = await call('vault', 'getEvents');
  check('tombstone respected', result.length === 1);

  console.log('\n--- hot-swap (M4): replace the revision, custody intact ---');
  {
    const swapTest = path.join(__dirname, '..', '..', '..', 'kairos-enclave', 'dist',
      'kairos-enclave-0.3.2-test.wasm32.ura');
    m.FS.mkdirTree('/urashell/services/vault/revisions');
    m.FS.writeFile('/urashell/services/vault/revisions/kairos-enclave-0.3.2-test.wasm32.ura',
      new Uint8Array(fs.readFileSync(swapTest)));
    m.FS.writeFile('/urashell/services/vault/revisions/kairos-enclave-0.3.1.wasm32.ura',
      new Uint8Array(fs.readFileSync(uraPath)));

    const swapped = JSON.parse(await ccall('iris_wasm_service_swap', 'string', ['string', 'string'],
      ['vault', '/wasm/kairos-enclave-0.3.2-test.wasm32.ura']));
    check('swap loads the new revision', swapped.ok === true && swapped.from === '0.3.1' && swapped.to === '0.3.2-test');
    check('custody transferred without a re-prompt', swapped.custody_transferred === true && swapped.unlocked === true);

    let st = await call('vault', 'status');
    check('new revision reports version + unlocked + store', st.version === 'kairos.enclave.v3-ura' &&
      st.unlocked === true && st.event_count === 1);
    let events = await call('vault', 'getEvents');
    check('events decrypt under the swapped revision (no unlock call)', Array.isArray(events) && events.length === 1);

    const rolled = JSON.parse(await ccall('iris_wasm_service_swap', 'string', ['string', 'string'],
      ['vault', '/wasm/kairos-enclave-0.3.1.wasm32.ura']));
    check('rollback swaps back', rolled.ok === true && rolled.to === '0.3.1' && rolled.unlocked === true);
    st = await call('vault', 'status');
    events = await call('vault', 'getEvents');
    check('rollback keeps custody + store', st.unlocked === true && Array.isArray(events) && events.length === 1);
  }

  console.log('\n--- service dir on disk ---');
  console.log('files:', m.FS.readdir('/urashell/services/vault').join(', '));
  check('vault.json persists', m.FS.readdir('/urashell/services/vault').includes('vault.json'));

  ccall('iris_wasm_service_close', null, ['string'], ['vault']);
  console.log(failures === 0 ? '\nPASS: vault smoke green' : `\nFAILURES: ${failures}`);
  process.exit(failures === 0 ? 0 : 1);
}).catch((error) => {
  console.error('vault smoke failed:', error);
  process.exit(1);
});
