/*
 * Portal app connect/disconnect behavioural smoke test
 * (run with: node tests/wifi_provisioning/portal_app_connect_test.js)
 *
 * Drives src/wifi_provisioning/web/app.js inside a minimal in-memory DOM and
 * verifies the TASK-126 Definition of Done:
 *
 *   1. Secured, open, and hidden-network connections submit credentials as a
 *      JSON request body (supports selected and manually entered SSIDs).
 *   2. Passwords are cleared from the DOM immediately after submission and are
 *      never retained across a later view switch or retry.
 *   3. Accepted credentials move to the connecting view; a connected status
 *      opens the connected/success view.
 *   4. A failed connection opens an actionable failure view, and retry returns
 *      to the password step for the same network without reloading.
 *   5. Disconnect sends DELETE /api/v1/wifi/connection and returns to network
 *      selection, leaving the provisioning portal reachable.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const assert = require('assert');

/* ------------------------------------------------------------------ */
/* Minimal DOM stubs (mirrors portal_app_test.js).                     */
/* ------------------------------------------------------------------ */

function makeStub(tag) {
  const children = [];
  return {
    tag,
    type: '',
    value: '',
    textContent: '',
    hidden: false,
    className: '',
    attributes: {},
    children,
    listeners: {},
    addEventListener(ev, fn) {
      (this.listeners[ev] = this.listeners[ev] || []).push(fn);
    },
    dispatch(ev, arg) {
      const ls = this.listeners[ev] || [];
      ls.slice().forEach((fn) => fn(arg || {}));
    },
    click() {
      this.dispatch('click');
    },
    focus() {},
    appendChild(c) {
      children.push(c);
      return c;
    },
    removeChild(c) {
      const i = children.indexOf(c);
      if (i >= 0) children.splice(i, 1);
      return c;
    },
    get firstChild() {
      return children.length ? children[0] : null;
    },
    setAttribute(k, v) {
      this.attributes[k] = v;
    },
    getAttribute(k) {
      return Object.prototype.hasOwnProperty.call(this.attributes, k)
        ? this.attributes[k] : null;
    },
  };
}

const documentStub = {
  views: [],
  elements: {},
  created: [],
  getElementById(id) {
    return this.elements[id] || null;
  },
  querySelectorAll(sel) {
    return sel === '.portal-view' ? this.views : [];
  },
  createElement(tag) {
    const s = makeStub(tag);
    this.created.push(s);
    return s;
  },
};

function seedElements() {
  const viewIds = [
    'view-network-list', 'view-password', 'view-connecting',
    'view-failure', 'view-connected',
  ];
  viewIds.forEach((vid) => {
    const v = makeStub('section');
    v.setAttribute('data-view', vid.replace('view-', ''));
    documentStub.elements[vid] = v;
    documentStub.views.push(v);
  });

  const simple = [
    'status-announcement', 'scan-progress', 'network-list',
    'network-list-empty', 'rescan-button', 'hidden-ssid-input',
    'hidden-ssid-next', 'password-form', 'password-input',
    'password-back-button', 'password-error', 'password-network-name',
    'connecting-network-name', 'connecting-cancel-button',
    'failure-message', 'failure-detail', 'failure-retry-button',
    'failure-back-button', 'connected-network-name',
    'connected-network-detail', 'connected-ip-detail',
    'connected-status-detail', 'connected-disconnect-button',
  ];
  simple.forEach((id) => {
    documentStub.elements[id] = makeStub('div');
  });

  documentStub.elements['network-list'].appendChild(
    documentStub.elements['network-list-empty']);
}
seedElements();

/* ------------------------------------------------------------------ */
/* Fetch + timers shims with in-flight tracking.                       */
/* ------------------------------------------------------------------ */

let timers = [];
const requestLog = [];
let activeRequests = [];

const realSetTimeout = setTimeout;
const realDelay = (ms) => new Promise((r) => realSetTimeout(r, ms));

globalThis.setTimeout = (fn, delay) => {
  timers.push({ fn, delay });
  return timers.length;
};
globalThis.clearTimeout = () => {};

async function fakeFetch(url, opts) {
  const task = { url, opts, handled: false };
  requestLog.push(task);
  activeRequests.push(task);
  return new Promise((resolve, reject) => {
    task.resolve = resolve;
    task.reject = reject;
  });
}
globalThis.fetch = fakeFetch;

function respond(task, payload) {
  task.handled = true;
  const i = activeRequests.indexOf(task);
  if (i >= 0) activeRequests.splice(i, 1);
  task.resolve({ ok: true, json: () => Promise.resolve(payload) });
}

function pendingRequests() {
  return requestLog.filter((t) => !t.handled);
}

function nextTick() {
  const t = timers.shift();
  return t.fn();
}

/* ------------------------------------------------------------------ */
/* Load the portal app once.                                           */
/* ------------------------------------------------------------------ */

const appSrc = fs.readFileSync(
  path.join(__dirname, '..', '..', 'src', 'wifi_provisioning', 'web', 'app.js'),
  'utf8');

globalThis.document = documentStub;
globalThis.window = { addEventListener() {} };

new Function(appSrc)();

async function settle() {
  await realDelay(0);
}

function onlyPending(url) {
  const list = pendingRequests();
  assert.strictEqual(list.length, 1, 'exactly one request in flight');
  assert.strictEqual(list[0].url, url, 'expected URL ' + url);
  return list[0];
}

/* Utility: complete a scan that yields the given networks, then return the
 * rendered network row buttons so a test can click one. */
async function scanToNetworks(networks) {
  requestLog.length = 0;
  activeRequests.length = 0;
  timers.length = 0;

  documentStub.elements['rescan-button'].click();
  await settle();
  onlyPending('/api/v1/wifi/scans');
  respond(pendingRequests()[0], {});
  await settle();
  onlyPending('/api/v1/wifi/networks');
  respond(pendingRequests()[0], { state: 'idle', networks });
  await settle();
  assert.strictEqual(pendingRequests().length, 0,
    'no leftover background requests after the scan');

  const listEl = documentStub.elements['network-list'];
  return listEl.children.filter(
    (c) => c.children && c.children[0] && c.children[0].tag === 'button');
}

/* The app kicks off an immediate auto-scan on page load. Drive it to
 * completion once so the scan-busy guard is clear before explicit scenarios. */
async function settleInitialScan() {
  onlyPending('/api/v1/wifi/scans');
  respond(pendingRequests()[0], {});
  await settle();
  onlyPending('/api/v1/wifi/networks');
  respond(pendingRequests()[0], { state: 'idle', networks: [] });
  await settle();
  requestLog.length = 0;
  activeRequests.length = 0;
  timers.length = 0;
}

/* Submit whatever is in the password field on the current view. */
async function submitPassword(password) {
  documentStub.elements['password-input'].value = password;
  documentStub.elements['password-form'].dispatch(
    'submit', { preventDefault() {} });
  await settle();
}

function credentialsBody(task) {
  return JSON.parse(task.opts.body);
}

/* The first status poll is issued immediately when the connection starts.
 * Respond "connecting" to keep the device working; the next poll is then
 * armed on the poll timer. */
async function reportConnecting() {
  const st = onlyPending('/api/v1/wifi/status');
  respond(st, { state: 'connecting', last_failure: '' });
  await settle();
}

/* Fire the armed status-poll timer and let the device report that it joined
 * the given network. */
async function reachConnectedTo(ssid) {
  const pending = pendingRequests();
  assert.strictEqual(timers.length, 1,
    'status poll re-armed while connecting');
  assert.strictEqual(pending.length, 0,
    'no immediate request while waiting on the poll timer');
  nextTick();
  await settle();
  const st = onlyPending('/api/v1/wifi/status');
  respond(st, {
    state: 'connected', ssid, ip: '192.168.1.5',
    netmask: '255.255.255.0', gateway: '192.168.1.1',
    last_failure: 'no_failure',
  });
  await settle();
}

/* ------------------------------------------------------------------ */
/* Test 1: secured-network connect (selected SSID) + password cleared. */
/* ------------------------------------------------------------------ */

async function runSecuredConnect() {
  const rows = await scanToNetworks([
    { ssid: 'Gamma', rssi: -60, channel: 11 },
  ]);
  assert.strictEqual(rows.length, 1, 'one network row rendered');

  rows[0].children[0].click();
  await settle();

  assert.strictEqual(
    documentStub.elements['view-password'].hidden, false,
    'selecting a network opens the password view');
  assert.strictEqual(
    documentStub.elements['password-network-name'].textContent, 'Gamma',
    'password step names the chosen SSID');

  await submitPassword('secret');
  const cred = onlyPending('/api/v1/wifi/credentials');
  const body = credentialsBody(cred);
  assert.strictEqual(body.ssid, 'Gamma', 'credentials carry the selected SSID');
  assert.strictEqual(body.password, 'secret', 'credentials carry the password');

  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'password cleared from the DOM immediately after submission');

  respond(cred, {});
  await settle();

  assert.strictEqual(
    documentStub.elements['view-connecting'].hidden, false,
    'accepted credentials move to the connecting view');
  assert.strictEqual(
    documentStub.elements['connecting-network-name'].textContent, 'Gamma',
    'connecting view names the target network');

  assert.strictEqual(pendingRequests().length, 1, 'status poll begins after submit');
  assert.strictEqual(pendingRequests()[0].url, '/api/v1/wifi/status',
    'status poll is the next request after credentials');
  respond(pendingRequests()[0], { state: 'connecting', last_failure: '' });
  await settle();

  await reachConnectedTo('Gamma');

  assert.strictEqual(
    documentStub.elements['view-connected'].hidden, false,
    'connected status opens the connected/success view');
  assert.strictEqual(
    documentStub.elements['connected-network-name'].textContent, 'Gamma',
    'connected view names the joined network');
  assert.strictEqual(
    documentStub.elements['connected-ip-detail'].textContent, '192.168.1.5',
    'connected view shows the assigned IP');
  assert.strictEqual(
    documentStub.elements['connected-status-detail'].textContent, 'Connected',
    'connected view reports Connected');
  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'password stays absent after reaching the connected view');
}

/* ------------------------------------------------------------------ */
/* Test 2: open network submits an empty password.                     */
/* ------------------------------------------------------------------ */

async function runOpenNetwork() {
  const rows = await scanToNetworks([
    { ssid: 'OpenNet', rssi: -70, channel: 6 },
  ]);

  rows[0].children[0].click();
  await settle();
  await submitPassword('');

  const cred = onlyPending('/api/v1/wifi/credentials');
  const body = JSON.parse(cred.opts.body);
  assert.strictEqual(body.ssid, 'OpenNet', 'open network SSID is submitted');
  assert.strictEqual(body.password, '', 'open network allows an empty password');
  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'empty password submission leaves the field clear');

  respond(cred, {});
  await settle();
  await reportConnecting();
  await reachConnectedTo('OpenNet');
}

/* ------------------------------------------------------------------ */
/* Test 3: hidden-network SSID enters the same connect workflow.       */
/* ------------------------------------------------------------------ */

async function runHiddenNetwork() {
  requestLog.length = 0;
  activeRequests.length = 0;
  timers.length = 0;

  documentStub.elements['hidden-ssid-input'].value = 'MyHiddenNet';
  documentStub.elements['hidden-ssid-next'].click();
  await settle();

  assert.strictEqual(
    documentStub.elements['view-password'].hidden, false,
    'manual hidden SSID opens the password view');
  assert.strictEqual(
    documentStub.elements['password-network-name'].textContent, 'MyHiddenNet',
    'hidden SSID is carried into the password step');

  await submitPassword('hunter2');
  const cred = onlyPending('/api/v1/wifi/credentials');
  const body = JSON.parse(cred.opts.body);
  assert.strictEqual(body.ssid, 'MyHiddenNet',
    'manually entered SSID is submitted');
  assert.strictEqual(body.password, 'hunter2',
    'hidden-network password is submitted');
  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'password cleared for hidden-network submission');

  respond(cred, {});
  await settle();
  assert.strictEqual(
    documentStub.elements['view-connecting'].hidden, false,
    'hidden network moves to connecting view');
  assert.strictEqual(
    documentStub.elements['connecting-network-name'].textContent, 'MyHiddenNet',
    'connecting view names the hidden network');

  await reportConnecting();
  await reachConnectedTo('MyHiddenNet');
}

/* ------------------------------------------------------------------ */
/* Test 4: failure is actionable and can be retried without reload.    */
/* ------------------------------------------------------------------ */

async function runFailureAndRetry() {
  const rows = await scanToNetworks([
    { ssid: 'Gamma', rssi: -60, channel: 11 },
  ]);

  rows[0].children[0].click();
  await settle();
  await submitPassword('wrongpass');

  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'password cleared even when the upcoming attempt will fail');

  const cred = onlyPending('/api/v1/wifi/credentials');
  respond(cred, {});
  await settle();

  const st = onlyPending('/api/v1/wifi/status');
  respond(st, { state: 'failed', last_failure: 'AUTHENTICATION' });
  await settle();

  assert.strictEqual(
    documentStub.elements['view-failure'].hidden, false,
    'failed status opens the failure view');
  assert.strictEqual(
    documentStub.elements['failure-detail'].textContent.indexOf('AUTHENTICATION') >= 0,
    true, 'failure view carries the failure reason');

  documentStub.elements['failure-retry-button'].click();
  await settle();

  assert.strictEqual(
    documentStub.elements['view-password'].hidden, false,
    'retry returns to the password view without reloading');
  assert.strictEqual(
    documentStub.elements['password-network-name'].textContent, 'Gamma',
    'retry keeps the same network');
  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'retry does not resurrect the previous password in the DOM');

  await submitPassword('correct');
  const cred2 = onlyPending('/api/v1/wifi/credentials');
  const body2 = JSON.parse(cred2.opts.body);
  assert.strictEqual(body2.ssid, 'Gamma', 'retried credentials re-select SSID');
  assert.strictEqual(body2.password, 'correct', 'retried credentials resubmit');
  assert.strictEqual(
    documentStub.elements['password-input'].value, '',
    'retried password cleared after resubmission');

  respond(cred2, {});
  await settle();
  assert.strictEqual(
    documentStub.elements['view-connecting'].hidden, false,
    'retried connection moves back to the connecting view');
  await reportConnecting();
  await reachConnectedTo('Gamma');
}

/* ------------------------------------------------------------------ */
/* Test 5: disconnect returns to network selection.                    */
/* ------------------------------------------------------------------ */

async function runDisconnect() {
  // Start from a fresh scan and join a network.
  const rows = await scanToNetworks([
    { ssid: 'Gamma', rssi: -60, channel: 11 },
  ]);
  rows[0].children[0].click();
  await settle();
  await submitPassword('');
  const cred = onlyPending('/api/v1/wifi/credentials');
  respond(cred, {});
  await settle();
  await reportConnecting();
  await reachConnectedTo('Gamma');
  assert.strictEqual(
    documentStub.elements['view-connected'].hidden, false,
    'connected before disconnect');

  documentStub.elements['connected-disconnect-button'].click();
  await settle();

  const del = onlyPending('/api/v1/wifi/connection');
  assert.strictEqual(del.opts.method, 'DELETE',
    'disconnect issues DELETE on the connection route');
  respond(del, { state: 'accepted' });
  await settle();

  assert.strictEqual(
    documentStub.elements['view-network-list'].hidden, false,
    'disconnect returns to the network selection view');
  assert.strictEqual(
    documentStub.elements['view-connected'].hidden, true,
    'connected view is left behind after disconnect');

  /* The portal stays usable: a fresh rescan still issues a scan request. */
  const scansBefore = requestLog.filter(
    (t) => t.url === '/api/v1/wifi/scans').length;
  documentStub.elements['rescan-button'].click();
  await settle();
  assert.strictEqual(
    requestLog.filter((t) => t.url === '/api/v1/wifi/scans').length,
    scansBefore + 1, 'portal stays reachable for a fresh scan after disconnect');
}

/* ------------------------------------------------------------------ */

(async function main() {
  await settleInitialScan();
  await runSecuredConnect();
  await runOpenNetwork();
  await runHiddenNetwork();
  await runFailureAndRetry();
  await runDisconnect();
  console.log('ALL PORTAL CONNECT TESTS PASSED');
})();