'use strict';

const $ = (selector) => document.querySelector(selector);
const byName = (name) => document.querySelector(`[name="${name}"]`);
const safeNumber = (value, fallback = 0) => Number.isFinite(Number(value)) ? Number(value) : fallback;

function showMessage(text, type = 'success') {
  const box = type === 'error' ? $('#errorMessage') : $('#message');
  box.textContent = text;
  box.classList.remove('hidden');
}

function queryMessage() {
  const msg = new URLSearchParams(location.search).get('msg');
  if (msg) showMessage(msg.replace(/\+/g, ' '));
}

function setBadge(online) {
  const badge = $('#connectionBadge');
  badge.textContent = online ? 'Online' : 'Offline';
  badge.className = `badge ${online ? 'online' : 'offline'}`;
}

function formatTemp(status) {
  if (status.systemTempValid) return `${safeNumber(status.systemTempC).toFixed(1)} °C`;
  if (status.systemTempFault) return 'Unavailable';
  return 'Validating';
}

function updateStatus(d) {
  setBadge(true);
  $('#deviceTime').textContent = d.time || 'Unknown';
  $('#powerStatus').textContent = `${d.usbPresent ? 'USB' : 'Battery'} · Input ${safeNumber(d.inputRailVoltage).toFixed(2)} V`;
  $('#displayStatus').textContent = d.backlightDimmed ? 'Dimmed' : 'Full brightness';
  $('#networkStatus').textContent = `${d.hostname || 'countdown'}.local · ${d.networkMode || 'Unknown'} · IP ${d.ipAddress || '—'} · Gateway ${d.gateway || '—'} · Subnet ${d.subnet || '—'} · DNS ${d.dns || '—'}`;
  $('#batteryStatus').textContent = d.batteryFound ? `${Math.round(safeNumber(d.batteryPercent))}% · ${safeNumber(d.batteryVoltage).toFixed(2)} V` : 'Not detected';
  $('#systemTempStatus').textContent = formatTemp(d);
  $('#luxStatus').textContent = d.lightSensorFound ? `${safeNumber(d.currentLux).toFixed(1)} lux · ${d.ambientDark ? 'Dark / dim eligible' : 'Bright'}` : 'Sensor not detected';

  const enabled = d.speakerAlertEnabled ?? true;
  const alarming = d.speakerAlertAlarming ?? d.buzzerAlarming;
  const muted = Boolean(d.speakerMuted);
  const ready = d.speakerReady !== false;
  const playing = Boolean(d.speakerPlaying);
  const playback = d.speakerPlayback || 'none';
  $('#speakerStatus').textContent = !enabled ? 'Disabled' :
    muted ? (alarming ? 'Muted — temperature alarm active' : 'Muted') :
    !ready ? 'Unavailable — codec or WAV file' :
    playback === 'celebration' ? 'Countdown celebration playing' :
    playback === 'temperature' ? 'Temperature alarm sounding' :
    d.celebrationPending ? 'Countdown celebration queued' :
    alarming ? 'Temperature alarm — waiting to repeat' :
    !d.systemTempValid ? 'Temperature alert inhibited — sensor unavailable' : 'Armed / OK';

  const muteButton = $('#speakerMuteButton');
  if (muteButton) {
    muteButton.dataset.muted = muted ? '1' : '0';
    muteButton.textContent = muted ? 'Unmute speaker' : 'Mute speaker';
  }

  const preview = $('#backgroundPreview');
  const image = $('#backgroundImage');
  const name = d.currentBgFile || '';
  $('#currentBackground').textContent = name || 'None (solid black)';
  if (name) {
    // Do not reload the JPEG on every 3-second status poll. Re-streaming a
    // large SD-hosted image through the synchronous WebServer blocks GPIO0
    // and touch polling for seconds on a slow or interrupted client.
    if (image.dataset.filename !== name) {
      image.dataset.filename = name;
      image.src = `/background?file=${encodeURIComponent(name)}`;
    }
    preview.classList.remove('hidden');
  } else {
    preview.classList.add('hidden');
    image.removeAttribute('src');
    delete image.dataset.filename;
  }
}

async function pollStatus() {
  try {
    const response = await fetch(`/status?_=${Date.now()}`, {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    updateStatus(await response.json());
  } catch (error) {
    console.error('Status update failed:', error);
    setBadge(false);
  }
}

function setValue(name, value) {
  const element = byName(name);
  if (element && value !== undefined && value !== null) element.value = String(value);
}

function populateConfig(c) {
  const t = c.target || {};
  setValue('ty', t.year); setValue('tmo', t.month); setValue('td', t.day);
  setValue('th', t.hour); setValue('tmi', t.minute); setValue('ts', t.second);
  setValue('eventText', c.countdownEventText || '');
  setValue('tz', c.timezone || 'UTC0');
  setValue('ntp1', c.ntpServer1 || 'pool.ntp.org');
  setValue('ntp2', c.ntpServer2 || 'time.nist.gov');

  const n = c.network || {};
  const hostnameInput = $('#hostnameInput');
  if (hostnameInput) hostnameInput.value = n.hostname || 'countdown';
  setValue('netMode', n.dhcp ? 'dhcp' : 'static');
  setValue('netIp', n.ip || '192.168.1.100');
  setValue('netGateway', n.gateway || '192.168.1.1');
  setValue('netSubnet', n.subnet || '255.255.255.0');
  setValue('netDns1', n.dns1 || '8.8.8.8');
  setValue('netDns2', n.dns2 || '1.1.1.1');

  setValue('dim', c.dimPercent);
  setValue('idle', c.idleTimeoutSec);
  setValue('timefmt', c.use12HourTime ? '12' : '24');
  setValue('dimLux', c.dimBelowLux);
  setValue('undimLux', c.undimAboveLux);

  // Firmware currently retains the legacy POST/config names internally.
  setValue('buzzEnabled', c.buzzerEnabled ? '1' : '0');
  setValue('speakerMuted', c.speakerMuted ? '1' : '0');
  setValue('buzzTripC', c.buzzerTripC);
  setValue('buzzClearC', c.buzzerClearC);

  setValue('bgMode', c.bgRotationEnabled ? 'rotate' : 'static');
  setValue('bgRotMin', c.bgRotationIntervalMin || 60);
  setValue('bgRotMode', c.bgRotationShuffle ? 'shuffle' : 'seq');

  const preset = $('#timezonePreset');
  const match = [...preset.options].find(option => option.value === c.timezone);
  preset.value = match ? c.timezone : '';
  preset.addEventListener('change', () => { if (preset.value) setValue('tz', preset.value); });

  loadBackgrounds(c.currentBgFile || '', Boolean(c.sdReady));
}

async function loadConfig() {
  try {
    const response = await fetch(`/api/config?_=${Date.now()}`, {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    populateConfig(await response.json());
  } catch (error) {
    console.error('Configuration load failed:', error);
    showMessage(`Configuration load failed: ${error.message}`, 'error');
  }
}

function backgroundRow(filename, current) {
  const row = document.createElement('div');
  row.className = 'background-option';
  const label = document.createElement('label');
  const radio = document.createElement('input');
  radio.type = 'radio'; radio.name = 'bgfile'; radio.value = filename; radio.checked = filename === current;
  const span = document.createElement('span');
  span.className = 'filename'; span.textContent = filename || 'None (solid black)';
  label.append(radio, span); row.append(label);

  if (filename) {
    const form = document.createElement('form');
    form.method = 'POST'; form.action = '/deleteBackground';
    form.addEventListener('submit', event => { if (!confirm(`Delete ${filename}?`)) event.preventDefault(); });
    const hidden = document.createElement('input');
    hidden.type = 'hidden'; hidden.name = 'file'; hidden.value = filename;
    const button = document.createElement('button');
    button.type = 'submit'; button.className = 'danger'; button.textContent = 'Delete';
    form.append(hidden, button); row.append(form);
  }
  return row;
}

async function loadBackgrounds(current, sdReady) {
  const list = $('#backgroundList');
  if (!sdReady) {
    list.innerHTML = '<p class="help">SD card not detected. Background management is unavailable.</p>';
    $('#backgroundCard').classList.add('disabled');
    return;
  }
  try {
    const response = await fetch(`/api/backgrounds?_=${Date.now()}`, {cache: 'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    list.replaceChildren(backgroundRow('', current));
    for (const file of (data.files || [])) list.append(backgroundRow(file, current));
  } catch (error) {
    list.innerHTML = `<p class="help">Unable to load backgrounds: ${error.message}</p>`;
  }
}

async function setSpeakerMute(muted) {
  const button = $('#speakerMuteButton');
  const message = $('#speakerActionMessage');
  if (button) button.disabled = true;
  if (message) message.textContent = muted ? 'Muting…' : 'Unmuting…';

  try {
    const body = new URLSearchParams({muted: muted ? '1' : '0'});
    const response = await fetch('/speaker/mute', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8'},
      body,
      cache: 'no-store'
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const result = await response.json();
    setValue('speakerMuted', result.speakerMuted ? '1' : '0');
    if (message) message.textContent = result.speakerMuted ? 'Speaker muted.' : 'Speaker unmuted.';
    await pollStatus();
  } catch (error) {
    if (message) message.textContent = `Unable to change mute state: ${error.message}`;
  } finally {
    if (button) button.disabled = false;
  }
}

function normalizeHostnameInput(value) {
  return String(value || '').trim().toLowerCase();
}

function validateHostname(value) {
  if (value.length < 1 || value.length > 32) return 'Hostname must be 1 to 32 characters.';
  if (value.startsWith('-') || value.endsWith('-')) return 'Hostname cannot begin or end with a hyphen.';
  if (!/^[a-z0-9-]+$/.test(value)) return 'Use only letters, numbers, and hyphens.';
  return '';
}

const saveHostnameButton = $('#saveHostnameButton');
if (saveHostnameButton) {
  saveHostnameButton.addEventListener('click', async () => {
    const input = $('#hostnameInput');
    const message = $('#hostnameMessage');
    const hostname = normalizeHostnameInput(input?.value);
    if (input) input.value = hostname;
    const validationError = validateHostname(hostname);
    if (validationError) {
      if (message) message.textContent = validationError;
      input?.focus();
      return;
    }
    if (!confirm(`Save hostname as ${hostname}.local and reboot now?`)) return;

    saveHostnameButton.disabled = true;
    if (message) message.textContent = 'Saving hostname and restarting…';
    try {
      const response = await fetch('/network/hostname', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8'},
        body: new URLSearchParams({hostname}),
        cache: 'no-store'
      });
      const result = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
      if (message) message.textContent = `Restarting as ${result.hostname}.local…`;
      setBadge(false);
    } catch (error) {
      if (message) message.textContent = `Unable to save hostname: ${error.message}`;
      saveHostnameButton.disabled = false;
    }
  });
}

const speakerMuteButton = $('#speakerMuteButton');
if (speakerMuteButton) {
  speakerMuteButton.addEventListener('click', () => {
    setSpeakerMute(speakerMuteButton.dataset.muted !== '1');
  });
}

const rebootSetupButton = $('#rebootSetupButton');
if (rebootSetupButton) {
  rebootSetupButton.addEventListener('click', async () => {
    if (!confirm('Reboot the clock into Setup Mode now? The current web connection will close.')) return;

    const message = $('#rebootSetupMessage');
    rebootSetupButton.disabled = true;
    if (message) message.textContent = 'Requesting Setup Mode…';

    try {
      const response = await fetch('/system/setup-mode', {
        method: 'POST',
        cache: 'no-store'
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      if (message) message.textContent = 'Restarting. Connect to the clock setup access point.';
      setBadge(false);
    } catch (error) {
      // A connection close immediately after the request can mean the restart
      // already began, so do not automatically treat it as a hard failure.
      if (message) message.textContent = 'Connection closed. The clock may already be restarting into Setup Mode.';
      setBadge(false);
    }
  });
}

$('#settingsForm').addEventListener('submit', event => {
  const dim = safeNumber(byName('dimLux').value);
  const undim = safeNumber(byName('undimLux').value);
  if (undim <= dim) {
    event.preventDefault();
    showMessage('Undim lux must be greater than dim lux.', 'error');
    byName('undimLux').focus();
    return;
  }
  const clear = safeNumber(byName('buzzClearC').value);
  const trip = safeNumber(byName('buzzTripC').value);
  if (clear >= trip) {
    event.preventDefault();
    showMessage('Speaker alert clear temperature must be below the alarm temperature.', 'error');
    byName('buzzClearC').focus();
  }
});

queryMessage();
loadConfig();
pollStatus();
setInterval(pollStatus, 3000);
