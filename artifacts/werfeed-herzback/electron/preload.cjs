const { contextBridge, ipcRenderer } = require('electron');

const COMMANDS = new Set([
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

function isPlainObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function validateCommand(command, payload) {
  if (!COMMANDS.has(command)) throw new TypeError('Unsupported native engine command.');
  if (payload === undefined && !['list_devices', 'start', 'stop'].includes(command)) {
    throw new TypeError('This native engine command requires an object payload.');
  }
  if (payload !== undefined && !isPlainObject(payload)) {
    throw new TypeError('Native engine command payload must be an object.');
  }
  if (payload !== undefined && Object.prototype.hasOwnProperty.call(payload, 'type')) {
    throw new TypeError('Native engine command payload may not override its type.');
  }
}

function subscribe(channel, listener) {
  if (typeof listener !== 'function') throw new TypeError('Engine listener must be a function.');
  const wrapped = (_event, value) => listener(value);
  ipcRenderer.on(channel, wrapped);
  return () => ipcRenderer.removeListener(channel, wrapped);
}

contextBridge.exposeInMainWorld('werfeedDesktop', {
  platform: process.platform,
  desktopShell: true,
  engine: Object.freeze({
    getStatus: () => ipcRenderer.invoke('werfeed-engine:status'),
    command: (command, payload) => {
      validateCommand(command, payload);
      return ipcRenderer.invoke('werfeed-engine:command', command, payload);
    },
    onEvent: (listener) => subscribe('werfeed-engine:event', listener),
    onStatus: (listener) => subscribe('werfeed-engine:status', listener),
    validation: Object.freeze({
      reportDevices: (devices, pairs) => {
        if (!Array.isArray(devices) || !Array.isArray(pairs)) {
          throw new TypeError('Invalid device validation report.');
        }
        ipcRenderer.send('werfeed-validation:devices', { devices, pairs });
      },
    }),
  }),
});