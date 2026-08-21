/*
 * Portal app behavioural smoke test (run with: node tests/.../portal_app_test.js)
 *
 * Drives src/wifi_provisioning/web/app.js inside a minimal in-memory DOM and
 * verifies the TASK-125 Definition of Done:
 *
 *   1. Simulated scan results render correctly (networks sorted by RSSI).
 *   2. Hostile SSID text is not interpreted as HTML (stays inert text).
 *   3. Polling never creates concurrent duplicate requests (single-flight).
 *   4. Connection status transitions update the correct view.
 */

'use strict';

const fs = require('fs');
const path = require('path');
const assert = require('assert');

/* ------------------------------------------------------------------ */
/* Minimal DOM stubs.                                                  */
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
/* Fetch + timers shims with in-flight tracking.                        */
/* ------------------------------------------------------------------ */

let timers = [];
const requestLog = [];
const activeRequests = [];
let maxConcurrent = 0;

// Preserve the real scheduler before app.js overrides setTimeout for the
// captive-portal fake clock, so the harness itself can await.
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
  maxConcurrent = Math.max(maxConcurrent, activeRequests.length);
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
/* Load the portal app.                                                */
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

/* ------------------------------------------------------------------ */
/* Test 1 + 2: scanning renders sorted, hostile stays inert text.       */
/* ------------------------------------------------------------------ */

async function runScanRender_andSafety() {
  onlyPending('/api/v1/wifi/scans');
  respond(pendingRequests()[0], {});
  await settle();
  onlyPending('/api/v1/wifi/networks');
  respond(pendingRequests()[0], { state: 'scanning', networks: [] });
  await settle();

  assert.strictEqual(timers.length, 1, 'poll re-armed while scanning');
  nextTick();
  await settle();
  onlyPending('/api/v1/wifi/networks');

  respond(pendingRequests()[0], {
    state: 'idle',
    networks: [
      { ssid: 'Alpha', rssi: -80, channel: 1 },
      { ssid: '<script>alert(1)</script>', rssi: -70, channel: 6 },
      { ssid: 'Gamma', rssi: -55, channel: 11 },
    ],
  });
  await settle();

  const listEl = documentStub.elements['network-list'];
  const rows = listEl.children.filter(
    (c) => c.children && c.children[0] && c.children[0].tag === 'button');
  const names = rows.map((li) => li.children[0].children[0].textContent);

  assert.deepStrictEqual(
    names, ['Gamma', '<script>alert(1)</script>', 'Alpha'],
    'networks are sorted by RSSI descending');

  assert.ok(
    !documentStub.created.some((c) => c.tag === 'script'),
    'no <script> element is ever created from an SSID');

  assert.strictEqual(
    rows[1].children[0].children[0].textContent,
    '<script>alert(1)</script>',
    'hostile SSID preserved as inert element text');

  assert.strictEqual(
    documentStub.elements['view-network-list'].hidden, false,
    'network list view is visible after a completed scan');
}

/* ------------------------------------------------------------------ */
/* Test 3: polling never overlaps duplicates.                           */
/* ------------------------------------------------------------------ */

async function runSingleFlight() {
  const scansBefore = requestLog.filter(
    (t) => t.url === '/api/v1/wifi/scans').length;

  documentStub.elements['rescan-button'].click();
  await settle();
  assert.strictEqual(
    requestLog.filter((t) => t.url === '/api/v1/wifi/scans').length,
    scansBefore + 1, 'rescan from idle issues one scan');

  const whileBusy = requestLog.filter(
    (t) => t.url === '/api/v1/wifi/scans').length;
  documentStub.elements['rescan-button'].click();
  await settle();
  assert.strictEqual(
    requestLog.filter((t) => t.url === '/api/v1/wifi/scans').length,
    whileBusy, 'rescan while a scan is pending does not stack a duplicate');

  respond(pendingRequests()[0], {});
  await settle();
  onlyPending('/api/v1/wifi/networks');

  documentStub.elements['rescan-button'].click();
  await settle();
  assert.strictEqual(pendingRequests().length, 1,
    'a poll in flight keeps async work single-flight');

  respond(pendingRequests()[0], { state: 'idle', networks: [] });
  await settle();

  assert.ok(maxConcurrent <= 1,
    'no concurrent duplicate requests were ever observed (' +
    maxConcurrent + ')');
}

/* ------------------------------------------------------------------ */
/* Test 4: status transitions switch to the matching view.              */
/* ------------------------------------------------------------------ */

async function runStatusTransitions() {
  // Start from a clean slate with a fresh scan that yields one network.
  requestLog.length = 0;
  activeRequests.length = 0;
  timers.length = 0;
  documentStub.elements['rescan-button'].click();
  await settle();
  onlyPending('/api/v1/wifi/scans');
  respond(pendingRequests()[0], {});
  await settle();
  onlyPending('/api/v1/wifi/networks');
  respond(pendingRequests()[0], {
    state: 'idle',
    networks: [{ ssid: 'Gamma', rssi: -60, channel: 11 }],
  });
  await settle();
  assert.strictEqual(pendingRequests().length, 0,
    'no leftover background requests after the scan');

  const listEl = documentStub.elements['network-list'];
  const rows = listEl.children.filter(
    (c) => c.children && c.children[0] && c.children[0].tag === 'button');
  rows[0].children[0].click();
  await settle();

  assert.strictEqual(
    documentStub.elements['view-password'].hidden, false,
    'selecting a network opens the password view');
  assert.strictEqual(
    documentStub.elements['password-network-name'].textContent, 'Gamma',
    'password step names the chosen SSID');

  documentStub.elements['password-input'].value = 'secret';
  documentStub.elements['password-form'].dispatch(
    'submit', { preventDefault() {} });
  await settle();

  onlyPending('/api/v1/wifi/credentials');
  respond(pendingRequests()[0], {});
  await settle();
  onlyPending('/api/v1/wifi/status');

  assert.strictEqual(
    documentStub.elements['view-connecting'].hidden, false,
    'accepted credentials move to the connecting view');

  respond(pendingRequests()[0], { state: 'connecting', last_failure: '' });
  await settle();
  assert.strictEqual(timers.length, 1, 'status poll re-armed while connecting');
  nextTick();
  await settle();
  onlyPending('/api/v1/wifi/status');

  respond(pendingRequests()[0], {
    state: 'connected', ssid: 'Gamma', ip: '192.168.1.5',
    netmask: '255.255.255.0', gateway: '192.168.1.1',
    last_failure: 'no_failure',
  });
  await settle();

  assert.strictEqual(
    documentStub.elements['view-connected'].hidden, false,
    'connected status opens the connected view');
  assert.strictEqual(
    documentStub.elements['connected-network-name'].textContent, 'Gamma',
    'connected view names the joined network');
  assert.strictEqual(
    documentStub.elements['connected-status-detail'].textContent, 'Connected',
    'connected view reports Connected');
}

/* ------------------------------------------------------------------ */

(async function main() {
  await runScanRender_andSafety();
  await runSingleFlight();
  await runStatusTransitions();
  console.log('ALL PORTAL APP TESTS PASSED');
})();
