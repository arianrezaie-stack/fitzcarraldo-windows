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
  'set_protection',
  'start_calibration',
]);
const ENGINE_EVENTS = new Set(['hello', 'devices', 'state', 'telemetry', 'calibration', 'error']);
const MAX_LINE_BYTES = 1024 * 1024;

let engine;
let engineStatus = { state: 'unavailable', reason: 'Engine has not been started.' };
let stoppingEngine = false;

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

function protocolError(message) {
  emitEngineEvent({ type: 'error', code: 'ENGINE_PROTOCOL_ERROR', message });
}

function handleEngineLine(line) {
  if (Buffer.byteLength(line) > MAX_LINE_BYTES) {
    protocolError('Engine sent a message larger than 1 MiB.');
    return;
  }

  let event;
  try {
    event = JSON.parse(line);
  } catch {
    protocolError('Engine sent invalid JSON.');
    return;
  }
  if (!event || typeof event !== 'object' || Array.isArray(event) ||
      !ENGINE_EVENTS.has(event.type)) {
    protocolError('Engine sent an unsupported event.');
    return;
  }
  emitEngineEvent(event);
}

function startEngine() {
  const binary = findEngineBinary();
  if (!binary) {
    setEngineStatus(
      'unavailable',
      `No native engine binary found for ${process.platform}-${process.arch}.`,
    );
    return;
  }

  stoppingEngine = false;
  setEngineStatus('starting');
  try {
    engine = spawn(binary, [], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
  } catch (error) {
    engine = undefined;
    setEngineStatus('unavailable', `Unable to launch native engine: ${error.message}`);
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
      if (line) handleEngineLine(line);
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
  });
  engine.once('exit', (code, signal) => {
    engine = undefined;
    if (stoppingEngine) {
      setEngineStatus('stopped');
    } else {
      setEngineStatus('crashed', `Native engine exited (${signal || `code ${code}`}).`);
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
ipcMain.handle('werfeed-engine:command', (_event, command, payload) => {
  if (!validCommand(command, payload)) {
    throw new Error('Invalid native engine command or payload.');
  }
  if (!engine || engineStatus.state !== 'running' || !engine.stdin.writable) {
    throw new Error(`Native engine is ${engineStatus.state}${engineStatus.reason ? `: ${engineStatus.reason}` : ''}`);
  }
  const message = JSON.stringify(payload === undefined ? { type: command } : { type: command, ...payload });
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

  if (isDevelopment) {
    const developmentUrl =
      process.env.WERFEED_DEV_URL || 'http://localhost:21230/';
    void window.loadURL(developmentUrl);
  } else {
    void window.loadFile(path.join(__dirname, '..', 'dist', 'public', 'index.html'));
  }
}

app.whenReady().then(() => {
  startEngine();
  createWindow();

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