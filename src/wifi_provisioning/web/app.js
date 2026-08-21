/*
 * HQ Wi-Fi Provisioning portal - scan and status client.
 *
 * This script is served by the captive portal's own HTTP listener from the
 * packed web assets, so it performs no external fetches and carries no
 * credentials. It talks only to the provisioning API on the same origin:
 *
 *   - POST   /api/v1/wifi/scans       - request an asynchronous scan;
 *   - GET    /api/v1/wifi/networks    - poll scan progress and AP records;
 *   - GET    /api/v1/wifi/status      - poll the connection state machine;
 *   - POST   /api/v1/wifi/credentials - submit an SSID/password pair;
 *   - DELETE /api/v1/wifi/connection  - request an asynchronous disconnect.
 *
 * Design notes (each maps to a portal requirement):
 *
 *   - Scan requests and network polls are serialized through a single "busy"
 *     guard so a slow response can never overlap a duplicate request. Pressing
 *     Rescan while a scan is in flight is a harmless no-op.
 *   - Connection status is polled with a bounded budget: a fixed interval and
 *     a maximum attempt count, so the page can never spin forever.
 *   - Timers are cancelled whenever the page is hidden (visibilitychange and
 *     pagehide) and re-armed only while the page is visible.
 *   - Network SSIDs are hostile by definition. They are only ever assigned to
 *     DOM nodes through textContent (or node creation) - never concatenated
 *     into markup - so a crafted "<script>..." SSID is inert text.
 *   - Passwords are read once at submit time, sent as a JSON request body, and
 *     cleared from the password field immediately. They are never retained in
 *     DOM state, echoed back, or logged, so a later view switch or retry never
 *     exposes the previous secret.
 *   - API and network failures are surfaced to the user through the live
 *     status region and the failure view instead of failing silently.
 */

'use strict';

(function portalApp() {
  /* Relative API endpoints on the provisioning listener. */
  var API_STATUS = '/api/v1/wifi/status';
  var API_SCANS = '/api/v1/wifi/scans';
  var API_NETWORKS = '/api/v1/wifi/networks';
  var API_CREDENTIALS = '/api/v1/wifi/credentials';
  var API_CONNECTION = '/api/v1/wifi/connection';

  /* Scan polling cadence while the device reports a scan in flight. */
  var SCAN_POLL_MS = 700;

  /* Bounded connection-state polling: at most STATUS_MAX_TRIES attempts spaced
   * STATUS_POLL_MS apart keeps the total wait small and deterministic. */
  var STATUS_POLL_MS = 1200;
  var STATUS_MAX_TRIES = 25;

  /* True while a user-initiated action (connection/disconnect) is being
   * tracked, so repeat clicks cannot queue duplicate work. */
  var requesting = false;

  /* True while a scan request or its network poll is in flight. Guards the
   * scan/poll cycle so concurrent duplicate requests never overlap. */
  var scanBusy = false;
  var scanTimer = null;

  /* Connection-status polling state. */
  var connActive = false;
  var connTimer = null;
  var connTries = 0;

  /* Page-visibility state; while hidden every timer is cleared. */
  var pageHidden = false;

  /* The SSID the user is currently working with (selected or typed). */
  var currentSsid = '';

  /* ------------------------------------------------------------------- */ 
  /* Small DOM helpers.                                                    */
  /* ------------------------------------------------------------------- */

  function byId(id) {
    return document.getElementById(id);
  }

  /* Focus the given input (used after switching views). */
  function focusId(id) {
    var el = byId(id);
    if (el && typeof el.focus === 'function') el.focus();
  }

  /* Assign text through textContent when the element exists. Always safe for
   * hostile input (SSID etc.) because textContent is inert element text. */
  function setText(id, text) {
    var el = byId(id);
    if (el) el.textContent = text;
  }

  /* Show exactly one .portal-view section, honouring the [hidden] attribute
   * the stylesheet uses to hide inactive steps. The section name matches the
   * data-view attribute on each section. */
  function showView(name) {
    var sections = document.querySelectorAll('.portal-view');
    var i;
    for (i = 0; i < sections.length; i++) {
      sections[i].hidden = sections[i].getAttribute('data-view') !== name;
    }
  }

  /* Publish a plain-text status into the hidden live region. Safe for any
   * input because the browser treats it as element text, never markup. */
  function announce(text) {
    var region = byId('status-announcement');
    if (region) region.textContent = text;
  }

  /* Show or hide the "scanning for networks" progress note. */
  function setScanProgress(active) {
    var note = byId('scan-progress');
    if (note) note.hidden = !active;
  }

  /* Show an API/network failure and stop background work. */
  function showNetworkError(message) {
    setScanProgress(false);
    byId('failure-message').textContent = message;
    byId('failure-detail').textContent = '';
    showView('failure');
    announce(message);
  }

  /* ------------------------------------------------------------------- */
  /* Scan request + poll (single-flight).                                 */
  /* ------------------------------------------------------------------- */

  function clearScanTimer() {
    if (scanTimer) {
      clearTimeout(scanTimer);
      scanTimer = null;
    }
  }

  /* Start the asynchronous scan, then poll the network snapshot. A scan is
   * only ever started when nothing else is in flight, and the follow-up poll
   * is driven from the one busy request, so requests cannot overlap. */
  function startScan() {
    var note;
    if (scanBusy || pageHidden) return;

    note = byId('scan-progress');
    if (note) note.hidden = false;
    announce('Scanning for available networks.');

    scanBusy = true;
    apiRequest('POST', API_SCANS, null)
      .then(function() {
        if (!scanBusy || pageHidden) return;
        setScanProgress(true);
        pollForNetworks();
      })
      .catch(function () {
        scanBusy = false;
        setScanProgress(false);
        showNetworkError('The device could not start a network scan. Check that provisioning is still running and try again.');
      });
  }

  /* Poll the networks route until the scan leaves the "scanning" state, then
   * render what was found. Each poll is launched only after the previous
   * response has arrived (we never schedule while scanBusy is set). */
  function pollForNetworks() {
    apiRequest('GET', API_NETWORKS, null)
      .then(function (json) {
        var isScanning;
        if (!scanBusy || pageHidden) return;
        isScanning = json && json.state === 'scanning';
        if (isScanning) {
          scheduleScanPoll(SCAN_POLL_MS);
        } else {
          scanBusy = false;
          setScanProgress(false);
          renderNetworks(json && json.networks ? json.networks : []);
        }
      })
      .catch(function () {
        scanBusy = false;
        setScanProgress(false);
        showNetworkError('The device could not read the network list. Check that provisioning is still available.');
      });
  }

  /* Put off one network poll (no-op while hidden; the page re-scans later or
   * the user presses Rescan). */
  function scheduleScanPoll(delayMs) {
    clearScanTimer();
    if (pageHidden) return;
    scanTimer = setTimeout(pollForNetworks, delayMs);
  }

  /* ------------------------------------------------------------------- */
  /* Rendering the network list.                                          */
  /* ------------------------------------------------------------------- */

  /* Render a list of access points into the network <ul>, newest/highest-RSSI
   * first. Every user-controlled value (SSID especially) is assigned with
   * textContent, never innerHTML. */
  function renderNetworks(list) {
    var listEl = byId('network-list');
    var empty = byId('network-list-empty');
    var sorted, i;

    if (!listEl) return;

    /* Remove the placeholder and any prior items. */
    while (listEl.firstChild) listEl.removeChild(listEl.firstChild);

    /* A network is only selectable if it has a non-empty SSID. */
    sorted = (list || []).filter(function (ap) {
      return ap && typeof ap.ssid === 'string' && ap.ssid.length > 0;
    });

    /* Sort descending by RSSI; stable so equal-RSSI networks keep their scan
     * order. Missing RSSI sorts last. */
    sorted.sort(function (a, b) {
      var ra = (typeof a.rssi === 'number') ? a.rssi : -200;
      var rb = (typeof b.rssi === 'number') ? b.rssi : -200;
      return rb - ra;
    });

    if (sorted.length === 0) {
      if (empty) {
        empty.textContent = 'No networks found yet. Press "Rescan for '
          + 'networks" to look for them.';
        listEl.appendChild(empty);
      }
      announce('No networks found.');
      return;
    }

    for (i = 0; i < sorted.length; i++) {
      var li = document.createElement('li');
      var item = buildNetworkItem(sorted[i]);
      li.appendChild(item);
      listEl.appendChild(li);
    }
    announce(sorted.length + (sorted.length === 1 ? ' network' : ' networks')
      + ' found.');
  }

  /* Build a selectable network row. The SSID is never interpolated into
   * markup; it is assigned only via node text properties. */
  function buildNetworkItem(ap) {
    var button = document.createElement('button');
    var main = document.createElement('span');
    var meta = document.createElement('span');
    var ssid = typeof ap.ssid === 'string' ? ap.ssid : '';

    button.type = 'button';
    button.className = 'network-option';

    /* Primary text: the SSID. Assigned via textContent, so hostile SSIDs are
     * inert text no matter how they are phrased. */
    main.className = 'network-name';
    main.textContent = ssid;

    /* Meta text: signal strength and channel (best-effort, never markup). */
    meta.className = 'network-meta';
    if (typeof ap.rssi === 'number') meta.textContent = ap.rssi + ' dBm';
    if (typeof ap.channel === 'number') {
      meta.textContent = meta.textContent ? meta.textContent + ' \u00b7 ch '
        + ap.channel : 'ch ' + ap.channel;
    }

    button.appendChild(main);
    button.appendChild(meta);

    button.addEventListener('click', function () {
      selectNetwork(ssid);
    });
    return button;
  }

  /* User picked a visible network: move to the password step. */
  function selectNetwork(ssid) {
    if (!ssid) return;
    currentSsid = ssid;
    showView('password');
    setPasswordLabel(ssid);
    clearPasswordError();
    byId('password-input').value = '';
    focusId('password-input');
  }

  /* ------------------------------------------------------------------ */
  /* Password / hidden-network entry.                                     */
  /* ------------------------------------------------------------------ */

  function setPasswordLabel(ssid) {
    var strong = byId('password-network-name');
    if (strong) strong.textContent = ssid;
  }

  function clearPasswordError() {
    var err = byId('password-error');
    if (err) {
      err.hidden = true;
      err.textContent = '';
    }
  }

  function showPasswordError(text) {
    var err = byId('password-error');
    if (err) {
      err.textContent = text;
      err.hidden = false;
    }
    announce(text);
  }

  /* "Next" on the hidden-network field: require a name, then move to the
   * password step for that network. */
  function takeHiddenSsid() {
    var input = byId('hidden-ssid-input');
    var ssid = input ? input.value : '';
    if (!ssid) {
      showPasswordError('Enter a network name to add a hidden network.');
      focusId('hidden-ssid-input');
      return;
    }
    currentSsid = ssid;
    showView('password');
    setPasswordLabel(ssid);
    clearPasswordError();
    byId('password-input').value = '';
    focusId('password-input');
  }

  /* ------------------------------------------------------------------ */
  /* Connection: submit credentials, then bounded status polling.         */
  /* ------------------------------------------------------------------ */

  function clearConnTimer() {
    if (connTimer) {
      clearTimeout(connTimer);
      connTimer = null;
    }
  }

  /* Validate the entry and hand the pair to the credentials API. */
  function connectFromForm(ev) {
    var password;
    ev.preventDefault();
    if (requesting) return;
    if (!currentSsid) {
      showPasswordError('No network was selected. Go back and choose one.');
      return;
    }
    password = byId('password-input').value;
    if (password.length > 64) {
      showPasswordError('The password is too long. Use no more than 64 '
        + 'characters.');
      return;
    }
    requestConnect(currentSsid, password);
  }

  /* Submit a connection request and move to the connecting view while the
   * device works asynchronously.
   *
   * The password is cleared from the DOM synchronously, before the request is
   * dispatched, so the secret is never retained once the user submits. It
   * stays empty on the password view, the connecting view, and every later
   * view (including a retry), which is why bad retries keep the field empty
   * and still work. */
  function requestConnect(ssid, password) {
    var passwordInput;
    if (requesting) return;
    requesting = true;
    /* Drop the secret immediately: once submitted, the password must not
     * remain in the DOM. This runs before the async request so the value is
     * gone even if the device later rejects or loses the connection. */
    passwordInput = byId('password-input');
    if (passwordInput) passwordInput.value = '';
    apiRequest('POST', API_CREDENTIALS, { ssid: ssid, password: password })
      .then(function () {
        requesting = false;
        connTries = 0;
        connActive = true;
        byId('connecting-network-name').textContent = ssid;
        showView('connecting');
        focusCancelOnConnect();
        announce('Connecting to ' + ssid);
        clearConnTimer();
        pollConnection();
      })
      .catch(function () {
        requesting = false;
        showView('password');
        showPasswordError('The device could not send your connection request. '
          + 'Check provisioning and try again.');
      });
  }

  function focusCancelOnConnect() {
    var btn = byId('connecting-cancel-button');
    if (btn && typeof btn.focus === 'function') btn.focus();
  }

  /* Poll /api/v1/wifi/status until the connection settles. The retry budget
   * is bounded by STATUS_MAX_TRIES, so a stuck or flapping device eventually
   * surfaces a clear failure instead of waiting forever. One request is in
   * flight at a time; the next attempt is armed only after a response (or a
   * timeout) arrives. */
  function pollConnection() {
    if (pageHidden || !connActive) return;

    apiRequest('GET', API_STATUS, null)
      .then(function (json) {
        var status = json && json.state;
        if (!connActive || pageHidden) return;
        if (status === 'connected') {
          connActive = false;
          showConnected(json);
          return;
        }
        if (status === 'failed') {
          connActive = false;
          showConnectionFailed(json);
          return;
        }
        connTries++;
        if (connTries >= STATUS_MAX_TRIES) {
          connActive = false;
          showNetworkError('The device did not confirm a connection. Try '
            + 'again or choose another network.');
          return;
        }
        scheduleConnPoll();
      })
      .catch(function () {
        if (!connActive || pageHidden) return;
        connTries++;
        if (connTries >= STATUS_MAX_TRIES) {
          connActive = false;
          showNetworkError('Lost touch with the device status API while '
            + 'connecting. Check provisioning and try again.');
          return;
        }
        scheduleConnPoll();
      });
  }

  /* Arm the next bounded connection poll, unless the page is hidden. */
  function scheduleConnPoll() {
    clearConnTimer();
    if (pageHidden) return;
    connTimer = setTimeout(pollConnection, STATUS_POLL_MS);
  }

  /* A live status response said the device failed to join the network. */
  function showConnectionFailed(json) {
    var category = json && json.last_failure;
    var message = 'This device could not join the selected network.';
    var detail = category && category !== 'no_failure'
      ? 'Reason: ' + category
      : '';
    byId('failure-message').textContent = message;
    byId('failure-detail').textContent = detail;
    showView('failure');
    announce(message + (detail ? ' ' + detail : ''));
  }

  /* ------------------------------------------------------------------ */
  /* Success and disconnect.                                              */
  /* ------------------------------------------------------------------ */

  function showConnected(json) {
    var ssid = json && json.ssid ? json.ssid : currentSsid;
    var ip = json && json.ip ? json.ip : '';

    /* Network name appears in the summary and the details list. Both are
     * assigned via textContent so a hostile SSID stays inert text. */
    setText('connected-network-name', ssid);
    setText('connected-network-detail', ssid);
    setText('connected-ip-detail', ip);
    setText('connected-status-detail', 'Connected');

    showView('connected');
    announce('Connected to ' + ssid);
  }

  function handleDisconnect() {
    if (requesting) return;
    requesting = true;
    apiRequest('DELETE', API_CONNECTION, null)
      .then(function () {
        requesting = false;
        resetToNetworkList();
      })
      .catch(function () {
        requesting = false;
        showNetworkError('The device could not disconnect. Check provisioning '
          + 'and try again.');
      });
  }

  /* Return to the network list view and clear staging state. */
  function resetToNetworkList() {
    connActive = false;
    clearConnTimer();
    requesting = false;
    currentSsid = '';
    showView('network-list');
    clearPasswordError();
  }

  /* Cancel an in-progress connection: stop polling and return to the list. */
  function cancelConnecting() {
    if (connActive) {
      connActive = false;
      clearConnTimer();
    }
    resetToNetworkList();
  }

  /* Retry the failed connection: return to password entry with the same SSID. */
  function retryFailure() {
    showView('password');
    setPasswordLabel(currentSsid);
    clearPasswordError();
    focusId('password-input');
  }

  /* ------------------------------------------------------------------ */
  /* Fetch wrapper.                                                       */
  /* ------------------------------------------------------------------ */

  function apiRequest(method, url, body) {
    var opts = {
      method: method,
      headers: { 'Content-Type': 'application/json' },
      cache: 'no-store'
    };
    if (body !== null) opts.body = JSON.stringify(body);
    return fetch(url, opts).then(function (resp) {
      if (!resp.ok) {
        throw new Error('http_' + resp.status);
      }
      return resp.json().catch(function () {
        /* 202 responses sometimes carry no parseable body. Treat an empty
         * payload as success when the request itself was accepted. */
        return null;
      });
    });
  }

  /* ------------------------------------------------------------------ */
  /* Startup wiring: bind the DOM, stop timers when hidden, auto-scan.    */
  /* ------------------------------------------------------------------ */

  /* Cancel any in-flight timers. Called whenever the page becomes hidden,
   * because a captive portal may sit in a phone/tablet background tab. */
  function pageBecameHidden() {
    pageHidden = true;
    clearScanTimer();
    clearConnTimer();
  }

  function pageBecameVisible() {
    pageHidden = false;
  }

  // Bind "visible" change and page teardown so backgrounded tabs stop polling.
  if (typeof document.addEventListener === 'function') {
    document.addEventListener('visibilitychange', function () {
      if (document.hidden) {
        pageBecameHidden();
      } else {
        pageBecameVisible();
      }
    });
  }
  if (typeof window.addEventListener === 'function') {
    window.addEventListener('pagehide', pageBecameHidden);
    window.addEventListener('pageshow', pageBecameVisible);
  }

  /* Bind every control once at load time. */
  function bindControls() {
    var rescanBtn = byId('rescan-button');
    var hiddenNext = byId('hidden-ssid-next');
    var passwordForm = byId('password-form');
    var passwordBackBtn = byId('password-back-button');
    var cancelBtn = byId('connecting-cancel-button');
    var retryBtn = byId('failure-retry-button');
    var failureBackBtn = byId('failure-back-button');
    var disconnectBtn = byId('connected-disconnect-button');
    var hiddenInput = byId('hidden-ssid-input');

    if (rescanBtn) rescanBtn.addEventListener('click', startScan);

    if (hiddenNext) {
      hiddenNext.addEventListener('click', takeHiddenSsid);
    }

    if (hiddenInput) {
      hiddenInput.addEventListener('keydown', function (ev) {
        // Enter on the hidden-SSID field advances to the password step.
        if (ev.key === 'Enter') {
          ev.preventDefault();
          takeHiddenSsid();
        }
      });
    }

    if (passwordForm) {
      passwordForm.addEventListener('submit', connectFromForm);
    }
    if (passwordBackBtn) {
      passwordBackBtn.addEventListener('click', backToNetworkList);
    }
    if (cancelBtn) {
      cancelBtn.addEventListener('click', cancelConnecting);
    }
    if (retryBtn) {
      retryBtn.addEventListener('click', retryFailure);
    }
    if (failureBackBtn) {
      failureBackBtn.addEventListener('click', backToNetworkList);
    }
    if (disconnectBtn) {
      disconnectBtn.addEventListener('click', handleDisconnect);
    }
  }

  // Human-readable alias used by the back/retry control bindings above.
  function backToNetworkList() {
    resetToNetworkList();
  }

  /* Show a network list and kick off the first scan so the page is
   * immediately useful when the captive assistant opens. */
  bindControls();
  showView('network-list');
  startScan();
})();