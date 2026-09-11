const { app, BrowserWindow, ipcMain, shell } = require('electron');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const isDevelopment = !app.isPackaged;
const ENGINE_COMMANDS = new Set([
  'list_devices',
  'configure',
  'start',
  'stop',
  'set_route_arming',
   'restart_audio',
  'set_protection',
  'set_manual_notch',
  'clear_manual_notch',
  'start_calibration',
   'reset_calibration',
]);
const ENGINE_EVENTS = new Set(['hello', 'devices', 'state', 'telemetry', 'calibration', 'calibration_reset', 'route_arming', 'error']);
const MAX_LINE_BYTES = 1024 * 1024;
const CALIBRATION_ANNOUNCEMENT = 'calibration-announcement.mp3';

let engine;
let engineStatus = { state: 'unavailable', reason: 'Engine has not been started.' };
let stoppingEngine = false;
let stdoutJsonBuffer = '';
const validationOutputPath = process.env.WERFEED_VALIDATION_OUTPUT;
const validationEngineOutputPath = process.env.WERFEED_VALIDATION_ENGINE_OUTPUT;
let validationFailed = false;
let validationFinished = false;

function writeValidation(record) {
  if (!validationOutputPath) return;
  try {
    fs.mkdirSync(path.dirname(validationOutputPath), { recursive: true });
    fs.appendFileSync(validationOutputPath, `${JSON.stringify(record)}\n`, 'utf8');
  } catch (error) {
    console.error(`[werfeed validation] unable to write evidence: ${error.message}`);
  }
}

function writeNativeOutput(line) {
  if (!validationEngineOutputPath) return;
  try {
    fs.mkdirSync(path.dirname(validationEngineOutputPath), { recursive: true });
    fs.appendFileSync(validationEngineOutputPath, `${line}\n`, 'utf8');
  } catch (error) {
    console.error(`[werfeed validation] unable to write native output: ${error.message}`);
  }
}

function finishValidation(exitCode, message) {
  if (!validationOutputPath || validationFinished) return;
  validationFinished = true;
  if (message) writeValidation({ type: 'validation_error', message });
  writeValidation({ type: 'validation_result', pass: exitCode === 0, exitCode });
  stopEngine();
  setTimeout(() => app.exit(exitCode), 100);
}

function failValidation(message) {
  if (!validationOutputPath || validationFailed) return;
  validationFailed = true;
  writeValidation({ type: 'validation_error', message });
  finishValidation(1);
}

function engineBinaryName() {
  return process.platform === 'win32' ? 'werfeed-engine.exe' : 'werfeed-engine';
}

function engineCandidates() {
  const binary = engineBinaryName();
  const platformDirectory = `${process.platform}-${process.arch}`;
  const candidates = [];

  // An override is useful for native-engine development, but is deliberately
  // ignored in packaged builds so an installed app only executes its bundle.
  if (isDevelopment && process.env.WERFEED_ENGINE_PATH) {
    candidates.push(process.env.WERFEED_ENGINE_PATH);
  }

  for (const root of [process.resourcesPath, app.getAppPath()]) {
    candidates.push(
      path.join(root, 'engine', platformDirectory, binary),
      path.join(root, 'engine', process.platform, binary),
      path.join(root, 'engine', binary),
      path.join(root, 'native', platformDirectory, binary),
    );
  }
  return [...new Set(candidates)];
}

function findEngineBinary() {
  return engineCandidates().find((candidate) => {
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      return true;
    } catch {
      return false;
    }
  });
}

function broadcast(channel, payload) {
  for (const window of BrowserWindow.getAllWindows()) {
    if (!window.isDestroyed()) window.webContents.send(channel, payload);
  }
}

function setEngineStatus(state, reason) {
  engineStatus = { state, ...(reason ? { reason } : {}) };
  broadcast('werfeed-engine:status', engineStatus);
}

function emitEngineEvent(event) {
  broadcast('werfeed-engine:event', event);
}

function protocolError(message, rawLine) {
  const detail = rawLine ? ` (${rawLine.slice(0, 240)})` : '';
  const fullMessage = `${message}${detail}`;
  emitEngineEvent({ type: 'error', code: 'ENGINE_PROTOCOL_ERROR', message: fullMessage });
  writeValidation({ type: 'engine_protocol_error', message: fullMessage });
  failValidation(`ENGINE_PROTOCOL_ERROR: ${fullMessage}`);
}

function handleEngineLine(line) {
  const normalized = line.replace(/^\uFEFF/, '').trim();
  if (!normalized) return;
  const candidate = stdoutJsonBuffer + normalized;
  if (!normalized.startsWith('{') && !stdoutJsonBuffer) {
    // JUCE/device drivers may print diagnostics despite the protocol contract.
    // Keep them out of the UI event stream, but preserve them in the desktop log.
    console.error(`[werfeed engine diagnostic] ${normalized}`);
    return;
  }
  if (Buffer.byteLength(candidate) > MAX_LINE_BYTES) {
    protocolError('Engine sent a message larger than 1 MiB.');
    stdoutJsonBuffer = '';
    return;
  }

  let event;
  try {
    event = JSON.parse(candidate);
  } catch {
    // Older native builds used JUCE pretty-printing. Keep accepting that
    // framing so a stale packaged engine cannot strand the device selector.
    if (candidate.startsWith('{') && !candidate.endsWith('}')) {
      stdoutJsonBuffer = candidate;
      return;
    }
    console.error('[werfeed engine] invalid JSON:', JSON.stringify(candidate.slice(0, 500)));
    protocolError('Engine sent invalid JSON.', candidate);
    stdoutJsonBuffer = '';
    return;
  }
  stdoutJsonBuffer = '';
  if (!event || typeof event !== 'object' || Array.isArray(event) ||
      !ENGINE_EVENTS.has(event.type)) {
    protocolError('Engine sent an unsupported event.', JSON.stringify(event));
    return;
  }
  emitEngineEvent(event);
}

function startEngine() {
  const binary = findEngineBinary();
  if (!binary) {
    const reason = `No native engine binary found for ${process.platform}-${process.arch}.`;
    setEngineStatus('unavailable', reason);
    failValidation(reason);
    return;
  }

  stoppingEngine = false;
  stdoutJsonBuffer = '';
  setEngineStatus('starting');
  try {
    engine = spawn(binary, [], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
  } catch (error) {
    engine = undefined;
    setEngineStatus('unavailable', `Unable to launch native engine: ${error.message}`);
    failValidation(`Unable to launch native engine: ${error.message}`);
    return;
  }

  let stdoutBuffer = '';
  engine.stdout.setEncoding('utf8');
  engine.stdout.on('data', (chunk) => {
    stdoutBuffer += chunk;
    let newline;
    while ((newline = stdoutBuffer.indexOf('\n')) !== -1) {
      const line = stdoutBuffer.slice(0, newline).replace(/\r$/, '');
      stdoutBuffer = stdoutBuffer.slice(newline + 1);
      if (line) {
        writeNativeOutput(line);
        handleEngineLine(line);
      }
    }
    if (Buffer.byteLength(stdoutBuffer) > MAX_LINE_BYTES) {
      stdoutBuffer = '';
      protocolError('Engine sent an unterminated message larger than 1 MiB.');
    }
  });
  engine.stderr.setEncoding('utf8');
  engine.stderr.on('data', (chunk) => console.error(`[werfeed engine] ${chunk.trimEnd()}`));
  engine.once('spawn', () => setEngineStatus('running'));
  engine.once('error', (error) => {
    engine = undefined;
    setEngineStatus('unavailable', `Unable to launch native engine: ${error.message}`);
    failValidation(`Unable to launch native engine: ${error.message}`);
  });
  engine.once('exit', (code, signal) => {
    engine = undefined;
    if (stdoutJsonBuffer) {
      protocolError('Native engine exited with an incomplete JSON frame.', stdoutJsonBuffer);
      stdoutJsonBuffer = '';
    }
    if (stoppingEngine) {
      setEngineStatus('stopped');
    } else {
      setEngineStatus('crashed', `Native engine exited (${signal || `code ${code}`}).`);
      failValidation(`Native engine exited (${signal || `code ${code}`}).`);
    }
  });
}

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function validCommand(command, payload) {
  if (!ENGINE_COMMANDS.has(command)) return false;
  if (payload === undefined) return command === 'list_devices' || command === 'start' || command === 'stop';
  return isPlainObject(payload) && !Object.prototype.hasOwnProperty.call(payload, 'type');
}

ipcMain.handle('werfeed-engine:status', () => engineStatus);
ipcMain.on('werfeed-validation:devices', (_event, payload) => {
  if (!validationOutputPath || validationFailed || validationFinished) return;
  const devices = Array.isArray(payload?.devices) ? payload.devices : [];
  const pairs = Array.isArray(payload?.pairs) ? payload.pairs : [];
  writeValidation({ type: 'renderer_devices', devices, pairs, pairCount: pairs.length });
  if (!devices.length || !pairs.length) {
    writeValidation({
      type: 'hardware_probe',
      available: false,
      message: !devices.length
        ? 'No Windows audio devices were present on the validation runner.'
        : 'Windows audio devices were present, but no compatible input/output pair was available.',
    });
    finishValidation(0);
    return;
  }
  writeValidation({ type: 'hardware_probe', available: true });
  finishValidation(0);
});
ipcMain.handle('werfeed-engine:command', (_event, command, payload) => {
  if (!validCommand(command, payload)) {
    throw new Error('Invalid native engine command or payload.');
  }
  if (!engine || engineStatus.state !== 'running' || !engine.stdin.writable) {
    throw new Error(`Native engine is ${engineStatus.state}${engineStatus.reason ? `: ${engineStatus.reason}` : ''}`);
  }
  const commandPayload = payload === undefined ? {} : { ...payload };
  if (command === 'start_calibration') {
    commandPayload.announcementPath = isDevelopment
      ? path.join(app.getAppPath(), 'public', CALIBRATION_ANNOUNCEMENT)
      : path.join(process.resourcesPath, CALIBRATION_ANNOUNCEMENT);
  }
  const message = JSON.stringify({ type: command, ...commandPayload });
  if (Buffer.byteLength(message) > MAX_LINE_BYTES) throw new Error('Native engine command is too large.');
  engine.stdin.write(`${message}\n`);
  return { accepted: true };
});

function stopEngine() {
  if (!engine) return;
  stoppingEngine = true;
  const child = engine;
  if (child.stdin.writable) child.stdin.end();
  child.kill('SIGTERM');
  const forceTimer = setTimeout(() => {
    if (engine === child) child.kill('SIGKILL');
  }, 3000);
  forceTimer.unref();
}

function createWindow() {
  const window = new BrowserWindow({
    width: 1440,
    height: 960,
    minWidth: 1024,
    minHeight: 720,
    backgroundColor: '#21130d',
    title: 'Werfeed Herzback',
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.cjs'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });

  window.webContents.setWindowOpenHandler(({ url }) => {
    void shell.openExternal(url);
    return { action: 'deny' };
  });

  window.webContents.on('did-fail-load', (_event, errorCode, errorDescription) => {
    const message = `Unable to load Werfeed Herzback (${errorCode}: ${errorDescription}).`;
    console.error(message);
    failValidation(message);
    void window.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(`
      <!doctype html>
      <html>
        <body style="margin:0;display:grid;place-items:center;min-height:100vh;background:#21130d;color:#eadbc2;font-family:Helvetica,Arial,sans-serif">
          <div style="max-width:560px;padding:32px;text-align:center">
            <h1 style="font-size:22px">Werfeed Herzback could not start</h1>
            <p style="color:#b9a187">${message}</p>
          </div>
        </body>
      </html>
    `)}`);
  });

  window.webContents.once('did-finish-load', () => {
    if (!engine && engineStatus.state !== 'starting') startEngine();
  });

  if (isDevelopment) {
    const developmentUrl =
      process.env.WERFEED_DEV_URL || 'http://localhost:21230/';
    void window.loadURL(developmentUrl);
  } else {
    void window.loadFile(path.join(__dirname, '..', 'dist', 'public', 'index.html'));
  }
}

app.whenReady().then(() => {
  writeValidation({
    type: 'package',
    packageVersion: app.getVersion(),
    electronVersion: process.versions.electron,
    platform: process.platform,
    architecture: process.arch,
  });
  createWindow();
  if (validationOutputPath) {
    setTimeout(() => failValidation('Timed out waiting for the renderer device selector.'), 30000).unref();
  }

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on('before-quit', stopEngine);

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});