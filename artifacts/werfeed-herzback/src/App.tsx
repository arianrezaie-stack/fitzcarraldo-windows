import { useEffect, useMemo, useRef, useState, type CSSProperties, type ReactNode } from 'react';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { Activity, AlertTriangle, AudioLines, BarChart3, CircleHelp, Gauge, LockKeyhole, Mic2, MoreHorizontal, Power, Radio, RefreshCw, RotateCcw, SlidersHorizontal, Timer, Volume2, Waves, X, Zap } from 'lucide-react';
import { ErrorBoundary } from '@/components/error-boundary';
import { Toaster } from '@/components/ui/toaster';
import { TooltipProvider } from '@/components/ui/tooltip';
import watermarkUrl from '@assets/wfhb-bg_1789115944261.png';
import packageJson from '../package.json';

const queryClient = new QueryClient();
const frequencyPosition = (frequency: number) => (Math.log10(frequency / 20) / Math.log10(20000 / 20)) * 100;
const spectrumTicks = [{ frequency: 20, label: '20 Hz' }, { frequency: 500, label: '500 Hz' }, { frequency: 2000, label: '2 kHz' }, { frequency: 8000, label: '8 kHz' }, { frequency: 20000, label: '20 kHz' }];

type Device = { deviceType: string; name: string; interfaceName?: string; direction: 'input' | 'output'; channels?: number; channelNames?: string[]; transport?: string; hardwareEligible?: boolean; channelMetadataReported?: boolean };
type DevicePair = { input: Device; output: Device; interfaceName: string; key: string };
type AudioChannelOption = { key: string; compatibilityKey: string; deviceType: string; deviceName: string; interfaceName: string; direction: 'input' | 'output'; channel: number; channelName: string; label: string };
type EngineStatus = { state: string; reason?: string };
type Notch = { frequency: number; depthDb: number; q: number };
type RouteSnapshot = { route: number; enabled?: boolean; suppression?: number; calibrated?: boolean; delayMs?: number; calibrationResponseDb?: number[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumAllowedNotches?: number; maximumCutDb?: number };
type Telemetry = { running?: boolean; sampleRate?: number; bufferSize?: number; callbackCpu?: number; xruns?: number; clockStability?: number; clockJitterMs?: number; inputPeak?: number; outputPeak?: number; protectionEnabled?: boolean; preset?: 'speech' | 'music'; calibrating?: boolean; calibrated?: boolean; calibratedRoutes?: boolean[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumCutDb?: number; routeTelemetry?: RouteSnapshot[] };
type AudioState = { phase?: string; running?: boolean; sampleRate?: number; bufferSize?: number };
type Route = { id: number; enabled: boolean; suppression: number; pairKey: string; inputKey: string; outputKey: string; inputChannel: number; outputChannel: number };
type CalibrationRecord = { delayMs?: number; responseDb: number[] };
type RecurringCutEvent = { frequency: number; timestamp: number };
type RecurringCutAlert = { route: number; frequency: number };
const recurringCutWindowMs = 10_000;
const recurringCutThreshold = 6;
const sameCutFrequency = (left: number, right: number) => Math.abs(Math.log2(left / right)) < 0.08;
const calibrationPeakProminenceDb = 3;
const calibrationHasPeakAtFrequency = (responseDb: number[] | undefined, frequency: number) => {
  if (!responseDb || responseDb.length < 5 || !Number.isFinite(frequency) || frequency <= 0) return false;
  const sorted = [...responseDb].sort((left, right) => left - right);
  const median = sorted[Math.floor(sorted.length / 2)];
  const position = Math.max(0, Math.min(1, Math.log10(frequency / 20) / Math.log10(1000)));
  const center = Math.round(position * (responseDb.length - 1));
  const start = Math.max(1, center - 2);
  const end = Math.min(responseDb.length - 2, center + 2);
  let peakIndex = start;
  for (let index = start + 1; index <= end; index += 1) {
    if (responseDb[index] > responseDb[peakIndex]) peakIndex = index;
  }
  const peak = responseDb[peakIndex];
  return peak - median >= calibrationPeakProminenceDb
    && peak >= responseDb[peakIndex - 1]
    && peak >= responseDb[peakIndex + 1];
};
const backendLabel = (deviceType: string) => {
  const normalized = deviceType.toLowerCase();
  if (normalized.includes('exclusive')) return 'WASAPI Exclusive';
  if (normalized.includes('windows audio')) return 'WASAPI Shared';
  if (normalized.includes('directsound')) return 'DirectSound';
  if (normalized.includes('asio')) return 'ASIO';
  return deviceType;
};
const backendPriority = (deviceType: string) => {
  const normalized = deviceType.toLowerCase();
  if (normalized.includes('exclusive')) return 0;
  if (normalized.includes('windows audio')) return 1;
  if (normalized.includes('asio')) return 2;
  if (normalized.includes('directsound')) return 3;
  return 4;
};
const interfaceName = (device: Device) => {
  if (device.interfaceName?.trim()) return device.interfaceName.trim();
  const name = device.name.trim();
  const match = name.match(/^(microphone|speakers?|line in|line out|headphones|digital audio|input|output)(?:\s+\d+)?\s*\((.+)\)$/i);
  return match?.[2]?.trim() || name;
};
const interfaceKey = (device: Device) => `${device.deviceType}\u0000${interfaceName(device).toLocaleLowerCase()}`;

function Badge({ children, tone = 'quiet' }: { children: ReactNode; tone?: 'quiet' | 'green' | 'amber' | 'red' }) {
  return <span className={`status-pill ${tone === 'green' ? 'live' : tone === 'red' ? 'alert' : ''}`}>{children}</span>;
}

function Meter({ level }: { level?: number }) {
  const safeLevel = typeof level === 'number' && Number.isFinite(level) ? Math.max(0, Math.min(100, level)) : 0;
  return <div className="meter" aria-label={level === undefined ? 'No measurement' : `${safeLevel.toFixed(1)}% level`}>{Array.from({ length: 18 }, (_, index) => <i key={index} className={index / 18 < safeLevel / 100 ? 'on' : ''} style={{ height: `${7 + (index % 4) * 2}px` }} />)}</div>;
}

function Spectrum({ values = [], notches = [] }: { values?: number[]; notches?: Notch[] }) {
  const [hoverReadout, setHoverReadout] = useState<{ x: number; y: number; frequency: number } | null>(null);
  const updateHover = (event: React.MouseEvent<HTMLDivElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const inset = 13;
    const width = Math.max(1, bounds.width - inset * 2);
    const plotX = Math.max(0, Math.min(width, event.clientX - bounds.left - inset));
    setHoverReadout({ x: plotX + inset, y: Math.max(0, Math.min(bounds.height, event.clientY - bounds.top)), frequency: Math.round(20 * Math.pow(1000, plotX / width)) });
  };
  const points = values.length > 1 ? values.map((value, index) => {
    const x = index / (values.length - 1) * 100;
    const y = Math.max(0, Math.min(100, (0 - value) / 100 * 100));
    return `${x},${y}`;
  }).join(' ') : '';
  return <div className="spectrum" data-testid="analyzer-spectrum" onMouseMove={updateHover} onMouseLeave={() => setHoverReadout(null)}>
    <div className="spectrum-grid" /><div className="spectrum-label">{points ? 'Live 256-bin detector · baseline-relative protection' : 'Awaiting native analyzer data'}</div>
    {points && <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Live spectrum"><polyline className="spectrum-trace" points={points} /></svg>}
    {notches.map((notch) => <i key={`${notch.frequency}-${notch.q}`} className="notch-marker" style={{ left: `${frequencyPosition(notch.frequency)}%` }} title={`${notch.frequency.toFixed(0)} Hz ${notch.depthDb.toFixed(1)} dB`} />)}
    <div className="spectrum-legend"><span><i className="legend-line" /> live spectrum</span><span><i className="legend-cut" /> {notches.length} adaptive cuts</span></div>
    <div className="spectrum-axis">{spectrumTicks.map((tick, index) => <span key={tick.frequency} className={index === 0 ? 'first' : index === spectrumTicks.length - 1 ? 'last' : ''} style={{ left: `${frequencyPosition(tick.frequency)}%` }}>{tick.label}</span>)}</div>
    {hoverReadout && <div className={`spectrum-hover-readout ${hoverReadout.x > 120 ? 'align-left' : ''}`} style={{ left: hoverReadout.x, top: hoverReadout.y }} aria-hidden="true">{hoverReadout.frequency.toLocaleString('en-US')} Hz</div>}
  </div>;
}

function TraceChart({ measured = [], live = [] }: { measured?: number[]; live?: number[] }) {
  const toPoints = (values: number[]) => values.length > 1 ? values.map((value, index) => {
    const x = index / (values.length - 1) * 100;
    const bounded = Math.max(-48, Math.min(18, value));
    const y = 100 - ((bounded + 48) / 66) * 100;
    return `${x},${y}`;
  }).join(' ') : '';
  const measuredPoints = toPoints(measured);
  const livePoints = toPoints(live);
  return <div className="trace-chart">
    <div className="trace-grid" />
    <div className="trace-y-axis"><span>+18</span><span>0</span><span>-24</span><span>-48 dB</span></div>
    <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Calibration frequency response comparison">
      {measuredPoints && <polyline className="trace-calibration" points={measuredPoints} />}
      {livePoints && <polyline className="trace-live" points={livePoints} />}
    </svg>
    <div className="trace-axis">{spectrumTicks.map((tick, index) => <span key={tick.frequency} className={index === 0 ? 'first' : index === spectrumTicks.length - 1 ? 'last' : ''} style={{ left: `${frequencyPosition(tick.frequency)}%` }}>{tick.label}</span>)}</div>
    <div className="trace-legend"><span><i className="trace-key calibration" /> calibrated room response</span><span><i className="trace-key live" /> current live spectrum</span></div>
  </div>;
}

function CalibrationDialog({ route, record, live, onClose }: { route: number; record?: CalibrationRecord; live?: number[]; onClose: () => void }) {
  if (!record) return null;
  return <div className="note-overlay" role="dialog" aria-modal="true" aria-labelledby="calibration-dialog-title">
    <div className="note-dialog calibration-dialog">
      <div className="dialog-heading"><div><div className="section-kicker"><BarChart3 size={14} /> route {route} measurement</div><h3 id="calibration-dialog-title">Calibration trace comparison</h3></div><button type="button" className="dialog-close" onClick={onClose} aria-label="Close calibration comparison"><X size={16} /></button></div>
      <p>The copper trace is the measured room and loudspeaker response. Its elevated regions receive more detection priority during live protection. The pale trace is the current route spectrum.</p>
      <TraceChart measured={record.responseDb} live={live} />
      <div className="dialog-readout"><span>Measured delay</span><strong>{record.delayMs === undefined ? '—' : `${record.delayMs.toFixed(1)} ms`}</strong></div>
      <button type="button" onClick={onClose}>Close comparison</button>
    </div>
  </div>;
}

function RouteMappingSummary({ input, output }: { input?: AudioChannelOption; output?: AudioChannelOption }) {
  const compatible = input && output && input.compatibilityKey === output.compatibilityKey;
  return <div className="route-mapping-summary">
    <div><span>input</span><strong>{input?.channelName ?? 'Not selected'}</strong></div>
    <div><span>output</span><strong>{output?.channelName ?? 'Not selected'}</strong></div>
    <p>{!input && !output ? 'Choose input and output in the audio engine panel' : compatible ? `${backendLabel(input.deviceType)} · ${input.interfaceName}` : 'Input/output interface mismatch'}</p>
  </div>;
}

function AudioMappingPanel({
  activeRoute,
  route,
  inputOptions,
  outputOptions,
  nativeBridgeAvailable,
  nativeReady,
  audioRunning,
  restartAvailable,
  restartingAudio,
  onInputChange,
  onOutputChange,
  onRestartAudio,
  sharedDescription,
  buffer,
  onBufferChange,
}: {
  activeRoute: number;
  route: Route;
  inputOptions: AudioChannelOption[];
  outputOptions: AudioChannelOption[];
  nativeBridgeAvailable: boolean;
  nativeReady: boolean;
  audioRunning: boolean;
  restartAvailable: boolean;
  restartingAudio: boolean;
  onInputChange: (key: string) => void;
  onOutputChange: (key: string) => void;
  onRestartAudio: () => void;
  sharedDescription: string;
  buffer: string;
  onBufferChange: (value: string) => void;
}) {
  return <div className="panel audio-mapping-panel">
    <div className="section-kicker"><LockKeyhole size={14} /> audio backend &amp; engine</div>
    <h3 className="section-title">Route {activeRoute + 1} audio mapping</h3>
    <p className="section-note">Choose the active route's mono input and output here. Switching routes loads that route's saved mapping; every armed route must still use one shared interface/backend.</p>
    <div className="mapping-controls">
      <label className="mapping-field"><span>Mono input</span><select value={route.inputKey} disabled={!nativeReady || !inputOptions.length} onChange={(event) => onInputChange(event.target.value)} aria-label={`Route ${activeRoute + 1} mono input`}>
        {inputOptions.length ? inputOptions.map((option) => <option key={option.key} value={option.key}>{option.label}</option>) : <option value="">{nativeBridgeAvailable ? 'No native mono inputs reported' : 'Open the Windows desktop app to map native audio'}</option>}
      </select></label>
      <label className="mapping-field"><span>Mono output</span><select value={route.outputKey} disabled={!nativeReady || !outputOptions.length} onChange={(event) => onOutputChange(event.target.value)} aria-label={`Route ${activeRoute + 1} mono output`}>
        {outputOptions.length ? outputOptions.map((option) => <option key={option.key} value={option.key}>{option.label}</option>) : <option value="">{nativeBridgeAvailable ? 'No native mono outputs reported' : 'Open the Windows desktop app to map native audio'}</option>}
      </select></label>
    </div>
    <div className="detail-row"><span>Shared interface basis</span><strong>{sharedDescription}</strong></div>
    <div className="detail-row"><span>Requested buffer</span><select value={buffer} disabled={!nativeReady} onChange={(event) => onBufferChange(event.target.value)}><option value="32">32 samples</option><option value="64">64 samples</option><option value="128">128 samples</option></select></div>
    <div className="detail-row"><span>Engine state</span><strong>{restartingAudio ? 'restarting selected audio connection…' : audioRunning ? 'running · bypassed until armed' : nativeReady ? 'connected · waiting for mappings' : 'unavailable'}</strong></div>
    <button type="button" className="restart-audio-button" disabled={!restartAvailable || restartingAudio} onClick={onRestartAudio}><RefreshCw size={14} /> {restartingAudio ? 'Restarting audio engine…' : 'Restart audio engine'}</button>
    <p className="section-note">The native engine receives the exact device names and channel indices selected here. Standby routes send no audio but remain selectable and mappable. Restart reopens the selected backend and interface without closing the app.</p>
  </div>;
}

function Home() {
  const bridge = typeof window !== 'undefined' ? window.werfeedDesktop?.engine : undefined;
  const [engineStatus, setEngineStatus] = useState<EngineStatus>(bridge ? { state: 'connecting' } : { state: 'unavailable', reason: 'Electron preload bridge is not present.' });
  const [devices, setDevices] = useState<Device[]>([]);
  const [devicesReported, setDevicesReported] = useState(false);
  const [audioState, setAudioState] = useState<AudioState>({});
  const [telemetry, setTelemetry] = useState<Telemetry>({});
  const [routes, setRoutes] = useState<Route[]>([
    { id: 1, enabled: true, suppression: 0.75, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
    { id: 2, enabled: true, suppression: 0.75, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
    { id: 3, enabled: true, suppression: 0.75, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
    { id: 4, enabled: true, suppression: 0.75, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
  ]);
  const [buffer, setBuffer] = useState('128');
  const [message, setMessage] = useState('');
  const [pendingStart, setPendingStart] = useState(false);
  const [restartingAudio, setRestartingAudio] = useState(false);
  const [preset, setPreset] = useState<'speech' | 'music'>('speech');
  const [activeRoute, setActiveRoute] = useState(0);
  const [calibrations, setCalibrations] = useState<Record<number, CalibrationRecord>>({});
  const [showCalibration, setShowCalibration] = useState(false);
  const [pendingCalibrationRoute, setPendingCalibrationRoute] = useState<number | null>(null);
  const [pendingCalibrationRestart, setPendingCalibrationRestart] = useState(false);
  const recurringCutHistory = useRef<Record<number, RecurringCutEvent[]>>({});
  const previousRouteNotches = useRef<Record<number, number[]>>({});
  const recurringCutAlertRef = useRef<RecurringCutAlert | null>(null);
  const [recurringCutAlert, setRecurringCutAlert] = useState<RecurringCutAlert | null>(null);
  const bypassInitialized = useRef(false);
  const clearRecurringCutSession = () => {
    recurringCutHistory.current = {};
    previousRouteNotches.current = {};
    recurringCutAlertRef.current = null;
    setRecurringCutAlert(null);
  };
  const dismissRecurringCutAlert = () => {
    const alert = recurringCutAlertRef.current;
    if (!alert) return;
    if (bridge && nativeReady) {
      void bridge.command('clear_manual_notch', {
        route: alert.route - 1,
        frequency: alert.frequency,
      }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to remove the permanent cut'));
    }
    recurringCutHistory.current[alert.route] = (recurringCutHistory.current[alert.route] ?? [])
      .filter((event) => !sameCutFrequency(event.frequency, alert.frequency));
    recurringCutAlertRef.current = null;
    setRecurringCutAlert(null);
  };

  const pairs = useMemo<DevicePair[]>(() => {
    const inputs = devices.filter((device) => device.direction === 'input');
    const outputs = devices.filter((device) => device.direction === 'output');
    const groups = new Map<string, { input?: Device; output?: Device; interfaceName: string }>();
    const preferDevice = (current: Device | undefined, candidate: Device) => !current || (candidate.channels ?? 0) > (current.channels ?? 0) ? candidate : current;
    inputs.forEach((input) => {
      const key = interfaceKey(input);
      const group = groups.get(key) ?? { interfaceName: interfaceName(input) };
      group.input = preferDevice(group.input, input);
      groups.set(key, group);
    });
    outputs.forEach((output) => {
      const key = interfaceKey(output);
      const group = groups.get(key) ?? { interfaceName: interfaceName(output) };
      group.output = preferDevice(group.output, output);
      groups.set(key, group);
    });
    return Array.from(groups.entries())
      .filter(([, group]) => group.input && group.output)
      .map(([key, group]) => ({ input: group.input!, output: group.output!, interfaceName: group.interfaceName, key }))
      .sort((a, b) => backendPriority(a.input.deviceType) - backendPriority(b.input.deviceType)
        || a.interfaceName.localeCompare(b.interfaceName));
  }, [devices]);
  const channelOptions = useMemo<AudioChannelOption[]>(() => devices.flatMap((device) => {
    // The native engine keeps named endpoints discoverable even when a Windows
    // driver withholds channel metadata until configure/open. Preserve that
    // compatibility behavior here instead of turning a valid endpoint into an
    // empty disabled selector.
    const channelCount = Math.max(1, Math.min(64, device.channels ?? 1));
    const compatibilityKey = interfaceKey(device);
    return Array.from({ length: channelCount }, (_, channel) => {
      const channelName = device.channelNames?.[channel]?.trim()
        || `${device.direction === 'input' ? 'Input' : 'Output'} channel ${channel + 1}`;
      return {
        key: `${device.deviceType}\u0000${device.name}\u0000${device.direction}\u0000${channel}`,
        compatibilityKey,
        deviceType: device.deviceType,
        deviceName: device.name,
        interfaceName: interfaceName(device),
        direction: device.direction,
        channel,
        channelName,
        label: `${device.transport ? `${device.transport} · ` : ''}${backendLabel(device.deviceType)} · ${interfaceName(device)} · ${channelName}`,
      };
    });
  }), [devices]);
  const inputOptions = useMemo(() => channelOptions.filter((option) => option.direction === 'input'), [channelOptions]);
  const outputOptions = useMemo(() => channelOptions.filter((option) => option.direction === 'output'), [channelOptions]);
  const nativeReady = !!bridge && engineStatus.state === 'running';
  const audioRunning = audioState.running ?? telemetry.running ?? false;
  const restartAvailable = nativeReady && (audioRunning
    || ['configured', 'stopped', 'device_stopped'].includes(audioState.phase ?? ''));
  const rate = telemetry.sampleRate ?? audioState.sampleRate;
  const actualBuffer = telemetry.bufferSize ?? audioState.bufferSize;
  const latency = rate && actualBuffer ? (actualBuffer * 2 / rate) * 1000 : undefined;
  const activeRouteState = routes[activeRoute] ?? routes[0];
  const routeSelection = (route: Route) => ({
    input: inputOptions.find((option) => option.key === route.inputKey),
    output: outputOptions.find((option) => option.key === route.outputKey),
  });
  const enabledRouteSelections = routes.filter((route) => route.enabled).map(routeSelection);
  const mappingCompatibilityKey = useMemo(() => {
    for (const route of routes) {
      const selection = routeSelection(route);
      const compatibilityKey = selection.input?.compatibilityKey ?? selection.output?.compatibilityKey;
      if (compatibilityKey) return compatibilityKey;
    }
    return undefined;
  }, [routes, inputOptions, outputOptions]);
   // Keep every native endpoint available while editing the active route.
   // The previous interface filter made it impossible to change interfaces
   // because the populated opposite selector narrowed this list.
   const scopedOptions = (options: AudioChannelOption[], _route: Route) => options;
  const sharedCompatibilityKey = enabledRouteSelections.length > 0 &&
    enabledRouteSelections.every((selection) => selection.input && selection.output
      && selection.input.compatibilityKey === selection.output.compatibilityKey
      && selection.input.compatibilityKey === enabledRouteSelections[0]?.input?.compatibilityKey)
    ? enabledRouteSelections[0]?.input?.compatibilityKey
    : undefined;
  const sharedInputOption = enabledRouteSelections.find((selection) => selection.input)?.input;
  const activeRouteTelemetry = telemetry.routeTelemetry?.find((route) => route.route === activeRoute + 1);
  const activeRouteCalibrated = Boolean(activeRouteTelemetry?.calibrated || calibrations[activeRoute + 1]);
  const activeCalibration = calibrations[activeRoute + 1] ?? (activeRouteTelemetry?.calibrationResponseDb ? {
    delayMs: activeRouteTelemetry.delayMs,
    responseDb: activeRouteTelemetry.calibrationResponseDb,
  } : undefined);
  const activeRouteSelection = routeSelection(activeRouteState);
  const activeRouteMapped = !!activeRouteSelection.input
    && !!activeRouteSelection.output
    && activeRouteSelection.input.compatibilityKey === activeRouteSelection.output.compatibilityKey;
  const activeSpectrum = activeRouteTelemetry?.spectrumDb ?? telemetry.spectrumDb;
   const activeNotches = activeRouteTelemetry?.notches ?? telemetry.notches;
   const activeNotchCapacity = activeRouteTelemetry?.maximumAllowedNotches ?? 6;
  const clockScore = audioRunning && typeof telemetry.clockStability === 'number' ? Math.max(0, Math.min(1, telemetry.clockStability)) : undefined;
  const clockLabel = clockScore === undefined ? '—' : clockScore >= 0.98 ? 'Stable' : clockScore >= 0.9 ? 'Watch' : 'Unstable';
  const clockTone = clockScore === undefined ? undefined : clockScore >= 0.98 ? 'stable' : clockScore >= 0.9 ? 'watch' : 'unstable';
  const protectionActive = telemetry.protectionEnabled === true;
   const showToast = (text: string) => { setMessage(text); window.setTimeout(() => setMessage(''), 2800); };
   const stopAudioForEdit = () => {
     if (!audioRunning || !bridge || !nativeReady) return;
     setPendingStart(false);
     void bridge.command('stop').catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to stop audio before editing'));
   };

  useEffect(() => {
    if (!bridge) return;
    const handleEvent = (event: unknown) => {
      if (!event || typeof event !== 'object' || !('type' in event)) return;
      const payload = event as Record<string, unknown>;
      if (payload.type === 'hello') showToast(`Native engine ${String(payload.engineVersion ?? '')} connected`);
      if (payload.type === 'devices' && Array.isArray(payload.devices)) {
        const records = payload.devices.filter((item): item is Device => !!item && typeof item === 'object' && typeof (item as Device).deviceType === 'string' && typeof (item as Device).name === 'string' && ((item as Device).direction === 'input' || (item as Device).direction === 'output'));
        setDevices(records);
        setDevicesReported(true);
      }
      if (payload.type === 'state') {
        const state = payload as AudioState & { type: string };
        setAudioState({ phase: state.phase, running: state.running, sampleRate: state.sampleRate, bufferSize: state.bufferSize });
        if (typeof state.running === 'boolean') setTelemetry((current) => ({ ...current, running: state.running }));
        if (state.phase === 'started') {
          setRestartingAudio(false);
          clearRecurringCutSession();
        }
        if (state.phase === 'configured' && pendingStart) {
          setPendingStart(false);
          void bridge.command('start').catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to start native audio'));
        }
      }
      if (payload.type === 'telemetry') setTelemetry(payload as Telemetry);
      if (payload.type === 'calibration') {
        const route = Number(payload.route ?? 0);
        const responseDb = Array.isArray(payload.responseDb)
          ? payload.responseDb.filter((value): value is number => typeof value === 'number' && Number.isFinite(value))
          : [];
        if (route > 0 && responseDb.length > 1) {
          setCalibrations((current) => ({ ...current, [route]: { delayMs: Number(payload.delayMs ?? 0), responseDb } }));
          setActiveRoute(route - 1);
        }
         void bridge.command('set_protection', { enabled: true, preset })
           .catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to arm protection after calibration'));
        showToast(`Route ${route || '—'} calibration saved · ${Number(payload.delayMs ?? 0).toFixed(1)} ms measured delay`);
      }
      if (payload.type === 'calibration_reset') {
        const route = Number(payload.route ?? 0);
        if (route > 0) {
          setCalibrations((current) => {
            const next = { ...current };
            delete next[route];
            return next;
          });
          if (route === activeRoute + 1) setShowCalibration(false);
          showToast(`Route ${route} calibration reset · ready for a new measurement`);
        }
      }
      if (payload.type === 'error') {
        setRestartingAudio(false);
        showToast(String(payload.message ?? 'Native engine error'));
      }
    };
    const removeStatus = bridge.onStatus((status) => setEngineStatus(status));
    const removeEvent = bridge.onEvent(handleEvent);
    void bridge.getStatus().then(setEngineStatus).catch((error: unknown) => setEngineStatus({ state: 'unavailable', reason: error instanceof Error ? error.message : 'Unable to read native engine status' }));
    return () => { removeStatus(); removeEvent(); };
   }, [bridge, pendingStart, activeRoute]);

  useEffect(() => {
    if (!bridge || engineStatus.state !== 'running') return;
    void bridge.command('list_devices').catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to list audio devices'));
  }, [bridge, engineStatus.state]);

  useEffect(() => {
    if (!bridge?.validation || !devicesReported) return;
    bridge.validation.reportDevices(devices, pairs.map(({ input, output }) => ({
      deviceType: input.deviceType,
      input: input.name,
      output: output.name,
    })));
  }, [bridge, devices, devicesReported, pairs]);

  useEffect(() => {
    setRoutes((items) => items.map((route) => {
      const input = inputOptions.find((option) => option.key === route.inputKey);
      const output = outputOptions.find((option) => option.key === route.outputKey);
      if ((input && output) || (!input && !output && !route.inputKey && !route.outputKey)) return route;
      return {
        ...route,
        pairKey: input?.compatibilityKey ?? output?.compatibilityKey ?? '',
        inputKey: input?.key ?? '',
        outputKey: output?.key ?? '',
        inputChannel: input?.channel ?? -1,
        outputChannel: output?.channel ?? -1,
      };
    }));
  }, [inputOptions, outputOptions]);
  const updateRouteChannel = (routeId: number, direction: 'input' | 'output', option: AudioChannelOption) => {
    setRoutes((items) => items.map((route) => {
      if (route.id !== routeId) return route;
      const current = routeSelection(route);
      const nextInput = direction === 'input' ? option : current.input;
      const nextOutput = direction === 'output' ? option : current.output;
       const compatible = !nextInput || !nextOutput || nextInput.compatibilityKey === nextOutput.compatibilityKey;
       // Preserve the side the user just selected. Only clear the opposite
       // side when switching interfaces, so a route can be remapped in two
       // deliberate selector changes.
       const input = compatible || direction === 'input' ? nextInput : undefined;
       const output = compatible || direction === 'output' ? nextOutput : undefined;
      return {
        ...route,
        pairKey: input?.compatibilityKey ?? output?.compatibilityKey ?? '',
        inputKey: input?.key ?? '',
        outputKey: output?.key ?? '',
        inputChannel: input?.channel ?? -1,
        outputChannel: output?.channel ?? -1,
      };
    }));
  };
  const updateActiveRouteChannel = (direction: 'input' | 'output', key: string) => {
    const options = scopedOptions(direction === 'input' ? inputOptions : outputOptions, activeRouteState);
    const option = options.find((candidate) => candidate.key === key);
     if (option) {
       stopAudioForEdit();
       updateRouteChannel(activeRoute + 1, direction, option);
     }
  };

  const configureAndStart = () => {
    if (!bridge || !nativeReady) return;
    const enabled = routes.filter((route) => route.enabled);
    if (!enabled.length) { showToast('Enable at least one mono route.'); return; }
    const configuredSelections = enabled.map(routeSelection);
    if (configuredSelections.some((selection) => !selection.input || !selection.output)) {
      showToast('Select an available mono input and output for every armed route.');
      return;
    }
    if (configuredSelections.some((selection) => selection.input!.compatibilityKey !== selection.output!.compatibilityKey)) {
      showToast('Each armed route must use an input and output from the same interface/backend.');
      return;
    }
    const firstSelection = configuredSelections[0]!;
    if (configuredSelections.some((selection) =>
      selection.input!.deviceType !== firstSelection.input!.deviceType
      || selection.input!.deviceName !== firstSelection.input!.deviceName
      || selection.output!.deviceName !== firstSelection.output!.deviceName)) {
      showToast('All armed routes must use the same interface and backend.');
      return;
    }
    const selectedInput = firstSelection.input!;
    const selectedOutput = firstSelection.output!;
    const inputChannelsRequired = Math.max(...configuredSelections.map((selection) => selection.input!.channel + 1));
    const outputChannelsRequired = Math.max(...configuredSelections.map((selection) => selection.output!.channel + 1));
    setPendingStart(true);
    void bridge.command('configure', {
      deviceType: selectedInput.deviceType,
      inputDevice: selectedInput.deviceName,
      outputDevice: selectedOutput.deviceName,
      sampleRate: 48000,
      bufferSize: Number(buffer),
       inputChannels: inputChannelsRequired,
       outputChannels: outputChannelsRequired,
      // Keep every route index stable. Disabled routes remain present as -1
      // entries so calibration and telemetry never shift to another route.
      routes: routes.map((route) => ({
         input: routeSelection(route).input?.channel ?? -1,
         output: routeSelection(route).output?.channel ?? -1,
        enabled: route.enabled,
        suppression: route.suppression,
      })),
    }).catch((error: unknown) => { setPendingStart(false); showToast(error instanceof Error ? error.message : 'Unable to configure native audio'); });
  };
  const autoStartReady = enabledRouteSelections.length > 0
    && enabledRouteSelections.every((selection) => selection.input && selection.output
      && selection.input.compatibilityKey === selection.output.compatibilityKey)
    && (() => {
      const first = enabledRouteSelections[0];
      return !!first.input && !!first.output && enabledRouteSelections.every((selection) =>
        selection.input?.deviceType === first.input?.deviceType
        && selection.input?.deviceName === first.input?.deviceName
        && selection.output?.deviceName === first.output?.deviceName);
    })();
  const startCalibration = (routeIndex: number) => {
    if (!bridge || !nativeReady || !audioRunning || telemetry.calibrating) return;
    setPendingCalibrationRoute(null);
    void bridge.command('start_calibration', { route: routeIndex, level: 0.06 })
      .catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to start calibration'));
  };
  useEffect(() => {
    if (!nativeReady || audioRunning || pendingStart || restartingAudio || pendingCalibrationRoute !== null || !autoStartReady) return;
    configureAndStart();
  }, [nativeReady, audioRunning, pendingStart, restartingAudio, pendingCalibrationRoute, autoStartReady]);
  useEffect(() => {
    if (pendingCalibrationRoute === null || !nativeReady || telemetry.calibrating) return;
    if (pendingCalibrationRestart) {
      if (audioRunning || pendingStart) return;
      setPendingCalibrationRestart(false);
      if (autoStartReady) configureAndStart();
      return;
    }
    if (audioRunning) {
      startCalibration(pendingCalibrationRoute);
      return;
    }
    if (pendingStart || audioState.phase === 'configured' || !autoStartReady) return;
    configureAndStart();
  }, [pendingCalibrationRoute, pendingCalibrationRestart, nativeReady, telemetry.calibrating, audioRunning, pendingStart, audioState.phase, autoStartReady]);
  useEffect(() => {
    if (!nativeReady || bypassInitialized.current) return;
    bypassInitialized.current = true;
    void bridge?.command('set_protection', { enabled: false, preset }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to set initial bypass'));
  }, [bridge, nativeReady, preset]);
  useEffect(() => {
    if (!nativeReady) {
      clearRecurringCutSession();
      return;
    }
    const now = Date.now();
    telemetry.routeTelemetry?.forEach((snapshot) => {
      const currentFrequencies = (snapshot.notches ?? [])
        .map((notch) => notch.frequency)
        .filter((frequency) => Number.isFinite(frequency) && frequency > 0);
      const previousFrequencies = previousRouteNotches.current[snapshot.route] ?? [];
      const freshEngagements = currentFrequencies.filter((frequency) =>
        !previousFrequencies.some((previous) => sameCutFrequency(previous, frequency)));
      const recentEvents = (recurringCutHistory.current[snapshot.route] ?? [])
        .filter((event) => now - event.timestamp <= recurringCutWindowMs);
      freshEngagements.forEach((frequency) => recentEvents.push({ frequency, timestamp: now }));
      recurringCutHistory.current[snapshot.route] = recentEvents;
      previousRouteNotches.current[snapshot.route] = currentFrequencies;

      if (recurringCutAlertRef.current) return;
      const firstQualifyingEvent = recentEvents.find((event) =>
        recentEvents.filter((candidate) => sameCutFrequency(candidate.frequency, event.frequency)).length >= recurringCutThreshold
        && snapshot.calibrated === true
        && calibrationHasPeakAtFrequency(snapshot.calibrationResponseDb, event.frequency));
      if (firstQualifyingEvent) {
        const alert = { route: snapshot.route, frequency: firstQualifyingEvent.frequency };
        recurringCutAlertRef.current = alert;
        setRecurringCutAlert(alert);
        void bridge?.command('set_manual_notch', {
          route: alert.route - 1,
          frequency: alert.frequency,
        }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to hold the recurring cut'));
      }
    });
  }, [nativeReady, telemetry.routeTelemetry]);
  const setProtection = (enabled: boolean, nextPreset = preset) => {
    if (!bridge || !nativeReady) return;
    setPreset(nextPreset);
    void bridge.command('set_protection', { enabled, preset: nextPreset }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change protection'));
  };
  const setRouteArming = (routeId: number, enabled: boolean) => {
    if (!enabled && routes.filter((route) => route.enabled).length <= 1) {
      showToast('Keep at least one route armed for audio protection.');
      return;
    }
    setRoutes((items) => items.map((route) => route.id === routeId ? { ...route, enabled } : route));
    if (!bridge || !nativeReady || !audioRunning) return;
    void bridge.command('set_route_arming', { route: routeId - 1, enabled }).catch((error: unknown) => {
      setRoutes((items) => items.map((route) => route.id === routeId ? { ...route, enabled: !enabled } : route));
      showToast(error instanceof Error ? error.message : 'Unable to change route arming');
    });
  };
  const restartAudio = () => {
    if (!bridge || !restartAvailable || restartingAudio) return;
    setRestartingAudio(true);
    void bridge.command('restart_audio').catch((error: unknown) => {
      setRestartingAudio(false);
      showToast(error instanceof Error ? error.message : 'Unable to restart audio engine');
    });
  };
  const setSuppression = (value: number) => {
    const nextValue = Math.max(0, Math.min(1, value));
    setRoutes((items) => items.map((route) => route.id === activeRoute + 1 ? { ...route, suppression: nextValue } : route));
    if (!bridge || !nativeReady || !audioRunning) return;
    void bridge.command('set_protection', { route: activeRoute, suppression: nextValue }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change route suppression'));
  };
  const calibrate = () => {
    if (!bridge || !nativeReady || telemetry.calibrating || !activeRouteMapped) return;
    const accepted = window.confirm('Calibration emits an audible impulse and sweep. Set speaker gain low, clear the room near the loudspeaker, and keep a physical mute ready. Start calibration?');
    if (!accepted) return;
    if (!activeRouteState.enabled) {
      setPendingCalibrationRoute(activeRoute);
      setPendingCalibrationRestart(true);
      stopAudioForEdit();
      setRoutes((items) => items.map((route) => route.id === activeRoute + 1
        ? { ...route, enabled: true }
        : route));
      return;
    }
    startCalibration(activeRoute);
  };
  const resetCalibration = () => {
    if (!bridge || !nativeReady || !activeCalibration || telemetry.calibrating) return;
    const accepted = window.confirm(`Clear the saved calibration for Route ${activeRoute + 1}? You can measure this route again afterward.`);
    if (!accepted) return;
    void bridge.command('reset_calibration', { route: activeRoute })
      .catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to reset calibration'));
  };
  const value = (number: number | undefined, digits = 1) => number === undefined ? '—' : number.toFixed(digits);
  const peakPercent = (peak: number | undefined) => peak === undefined ? undefined : peak * 100;
  const activeInputOptions = scopedOptions(inputOptions, activeRouteState);
  const activeOutputOptions = scopedOptions(outputOptions, activeRouteState);

  const shellStyle = { '--watermark-image': `url("${watermarkUrl}")` } as CSSProperties;
  return <div className="app-shell" style={shellStyle}>
    {message && <div className="toast" role="status"><AlertTriangle size={14} /> {message}</div>}
    <header className="app-header">
       <div className="brand-lockup"><div className="brand-mark"><AudioLines size={20} /></div><div><div className="eyebrow">Arian Rezaie's Adaptive Feedback Control</div><h1 className="brand-title">Werfeed Herzback <span className="brand-byline">· by Arian Rezaie</span> <small className="brand-version">v{packageJson.version}</small></h1></div></div>
      <div className="header-meta">
        <Badge tone={nativeReady ? 'green' : 'red'}><span className="route-dot" /> {bridge ? `Engine ${engineStatus.state}` : 'Native engine unavailable'}</Badge>
        <span className="session">{engineStatus.reason ?? 'Native engine status'}</span>
        <button type="button" className={`header-bypass ${protectionActive ? '' : 'active'}`} disabled={!nativeReady} onClick={() => setProtection(!protectionActive)}><Power size={13} /> {protectionActive ? 'Bypass suppression' : 'Suppression bypassed'}</button>
      </div>
    </header>
    <main className="main-content section-stack">
       <AudioMappingPanel activeRoute={activeRoute} route={activeRouteState} inputOptions={activeInputOptions} outputOptions={activeOutputOptions} nativeBridgeAvailable={!!bridge} nativeReady={nativeReady} audioRunning={audioRunning} restartAvailable={restartAvailable} restartingAudio={restartingAudio} onInputChange={(key) => updateActiveRouteChannel('input', key)} onOutputChange={(key) => updateActiveRouteChannel('output', key)} onRestartAudio={restartAudio} sharedDescription={sharedCompatibilityKey && sharedInputOption ? `${backendLabel(sharedInputOption.deviceType)} · ${sharedInputOption.interfaceName}` : mappingCompatibilityKey ? 'Other routes limited to the selected interface' : !bridge ? 'Open the Windows desktop app to access native audio devices' : inputOptions.length && outputOptions.length ? 'Choose an input and output on a route to lock the shared clock' : devicesReported ? 'Windows reported no input/output endpoints for the compiled audio backends' : 'Waiting for native interface discovery'} buffer={buffer} onBufferChange={(value) => { stopAudioForEdit(); setBuffer(value); }} />
      <section className="panel routing-panel">
        <div className="panel-heading"><div><div className="section-kicker"><SlidersHorizontal size={14} /> mono routing</div><h3 className="section-title">Four simultaneous routes</h3><p className="section-note">Choose a route to edit its mapping above. The first selected interface limits every other route so all armed paths share one hardware clock.</p></div></div>
          <div className="route-grid">{routes.map((route) => { const routeInfo = telemetry.routeTelemetry?.find((item) => item.route === route.id); const calibrated = routeInfo?.calibrated || Boolean(calibrations[route.id]); const selection = routeSelection(route); const mapped = Boolean(selection.input && selection.output && selection.input.compatibilityKey === selection.output.compatibilityKey); const routeStatus = !route.enabled ? 'no audio I/O' : !mapped ? 'mapping required' : !audioRunning ? 'audio stopped' : protectionActive ? 'suppression active' : 'audio routed · bypassed'; return <div className={`route-card ${route.id === activeRoute + 1 ? 'selected' : ''} ${route.enabled ? '' : 'muted'}`} key={route.id}><button type="button" className="route-card-top" disabled={!nativeReady} onClick={() => setActiveRoute(route.id - 1)} aria-pressed={route.id === activeRoute + 1}><span className={`route-dot ${route.enabled ? 'locked' : ''}`} /><span className="channel">ROUTE {route.id}</span><span className="route-select-label">{route.id === activeRoute + 1 ? 'viewing' : 'select'}</span></button><div className="route-name">Route {route.id} mono bus pair</div><RouteMappingSummary input={selection.input} output={selection.output} /><div className="route-meta"><span>{route.enabled ? 'armed path' : 'standby · mappable'}</span><label className="route-arm"><input type="checkbox" checked={route.enabled} disabled={!nativeReady} onChange={() => setRouteArming(route.id, !route.enabled)} /><span>arm</span></label></div><div className="route-card-bottom"><span>{routeStatus}</span><span>{calibrated ? 'measured baseline' : mapped ? 'flat baseline ready' : 'flat baseline pending'}</span></div></div>; })}</div>
      </section>
      <section className="panel calibration-panel">
          <div className="panel-heading"><div><div className="section-kicker"><BarChart3 size={14} /> calibration</div><h3 className="section-title">Measure one route at a time</h3><p className="section-note">Calibration is optional. Uncalibrated routes use a virtual flat frequency baseline for basic suppression; a measurement adds room-specific peak bias and hold timing.</p></div><Badge tone={telemetry.calibrating ? 'amber' : activeRouteCalibrated ? 'quiet' : 'green'}>{telemetry.calibrating ? 'sweep in progress' : activeRouteCalibrated ? 'measured baseline saved' : activeRouteMapped ? 'virtual flat baseline' : 'configure io'}</Badge></div>
         <div className="calibration-controls"><label className="mapping-field"><span>Calibration route</span><select value={activeRoute} disabled={!nativeReady || !audioRunning || telemetry.calibrating} onChange={(event) => setActiveRoute(Number(event.target.value))}>{routes.map((route) => <option key={route.id} value={route.id - 1}>Route {route.id}{!route.enabled ? ' · standby' : ''}</option>)}</select></label><button type="button" className="plain-button footer-bypass" disabled={!nativeReady || !audioRunning || telemetry.calibrating || !activeRouteMapped} onClick={calibrate}><CircleHelp size={14} /> {telemetry.calibrating ? 'Calibrating…' : !activeRouteState.enabled ? `Arm & calibrate Route ${activeRoute + 1}` : `Calibrate Route ${activeRoute + 1}`}</button><button type="button" className="plain-button calibration-reset-button" disabled={!nativeReady || !activeCalibration || telemetry.calibrating} onClick={resetCalibration}><RotateCcw size={14} /> Reset Route {activeRoute + 1}</button><button type="button" className="calibration-trace-button" disabled={!activeCalibration || !nativeReady} onClick={() => setShowCalibration(true)} aria-label={`View Route ${activeRoute + 1} calibration measurement`} title={activeCalibration ? `View Route ${activeRoute + 1} measurement` : 'Calibrate this route to view its measurement'}><MoreHorizontal size={17} /></button></div>
      </section>
       <section className="route-focus-stack">
           <section className="panel route-analyzer-panel"><div className="panel-heading"><div><div className="section-kicker"><Radio size={14} /> route {activeRoute + 1} analyzer</div><h3 className="section-title">Live spectrum and adaptive cuts</h3><p className="section-note">The detector uses a virtual flat reference until calibration adds measured room weighting. Armed routes share 32 adaptive notch slots; disarmed routes return their share to the remaining routes.</p></div><span className="slot-count">{activeNotches?.length ?? 0} / {activeNotchCapacity} cuts</span></div><Spectrum values={activeSpectrum} notches={activeNotches} />{recurringCutAlert?.route === activeRoute + 1 && <div className="recurring-cut-alert" role="status" aria-live="polite"><span>Permanent system cut held at <strong>{recurringCutAlert.frequency.toLocaleString('en-US', { maximumFractionDigits: 0 })} Hz</strong> until you press X.</span><button type="button" className="recurring-cut-dismiss" onClick={dismissRecurringCutAlert} aria-label={`Remove permanent recurring ${Math.round(recurringCutAlert.frequency)} Hz cut`} title="Remove this permanent cut"><X size={15} /></button></div>}</section>
        <section className={`panel route-protection-panel ${protectionActive ? '' : 'is-bypassed'}`}><div className="panel-heading"><div><div className="section-kicker"><SlidersHorizontal size={14} /> route {activeRoute + 1} protection</div><h3 className="section-title">{protectionActive ? 'Suppression armed' : 'Suppression bypassed'}</h3><p className="section-note">Mapped and armed routes receive basic suppression from the flat baseline immediately. Calibration now adds stronger measured-peak bias and deeper protection. The previous maximum depth is reached at 80%; the last 20% adds extra sensitivity, depth, and permanent repeating-cut retention.</p></div><Badge tone={protectionActive ? 'green' : 'red'}>{protectionActive ? 'armed' : 'bypassed'}</Badge></div><div className="mode-switch"><button type="button" className={preset === 'speech' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(protectionActive, 'speech')}><Mic2 size={13} /> Speech</button><button type="button" className={preset === 'music' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(protectionActive, 'music')}><Waves size={13} /> Music</button></div><div className="route-fader"><div className="fader-copy"><div className="curve-name"><SlidersHorizontal size={14} /> Suppression amount</div><p className="section-note">At 80%, protection reaches the former −24 dB maximum. At 100%, protection can reach −32 dB; above 80%, 3–4 repeating peaks are held permanently, and at 100% up to 8 repeating peaks can remain cut.</p></div><div className="fader-control"><div className="fader-scale"><span>light · 0 dB</span><span>deep · −32 dB</span></div><input className="suppression-slider" type="range" min="0" max="100" step="1" value={Math.round((activeRouteState?.suppression ?? 0.75) * 100)} disabled={!nativeReady} onChange={(event) => setSuppression(Number(event.target.value) / 100)} aria-label={`Route ${activeRoute + 1} suppression amount`} style={{ background: `linear-gradient(90deg, #d19a63 0%, #d19a63 ${Math.round((activeRouteState?.suppression ?? 0.75) * 100)}%, #4a3025 ${Math.round((activeRouteState?.suppression ?? 0.75) * 100)}%, #4a3025 100%)` }} /><strong>{Math.round((activeRouteState?.suppression ?? 0.75) * 100)}%</strong></div></div></section>
       <section className="panel telemetry-panel">
        <div className="panel-heading"><div><div className="section-kicker"><Gauge size={14} /> live readings</div><h3 className="section-title">Engine and signal health</h3><p className="section-note">Telemetry stays visible below the control sections so live operation can be monitored without moving the routing controls.</p></div><Badge tone={audioRunning ? 'green' : 'quiet'}>{audioRunning ? 'audio running' : nativeReady ? 'engine connected' : 'waiting for engine'}</Badge></div>
        <div className="metrics">{[{ label: 'Callback CPU', value: value(telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100), unit: '%', icon: <Activity size={14} />, level: telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100 }, { label: 'Round-trip buffer', value: value(latency, 2), unit: 'ms est.', icon: <Gauge size={14} />, level: undefined }, { label: 'XRuns', value: telemetry.xruns === undefined ? '—' : String(telemetry.xruns), unit: 'local est.', icon: <Volume2 size={14} />, level: undefined }, { label: 'Clock stability', value: clockLabel, unit: clockScore === undefined ? '' : `${value(telemetry.clockJitterMs, 2)} ms jitter`, icon: <Timer size={14} />, level: clockScore === undefined ? undefined : clockScore * 100, tone: clockTone }].map((metric) => <div className="metric" key={metric.label}><div className="metric-head"><span>{metric.label}</span>{metric.icon}</div><div className={`metric-value ${metric.tone ?? ''}`}>{metric.value} <small>{metric.unit}</small></div><Meter level={metric.level} /></div>)}</div>
        <div className="health-grid"><div className="health"><label>Sample rate</label><strong>{rate === undefined ? '—' : `${rate} Hz`}</strong></div><div className="health"><label>Buffer</label><strong>{actualBuffer === undefined ? '—' : `${actualBuffer} samples`}</strong></div><div className="health"><label>Input peak</label><strong>{peakPercent(telemetry.inputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.inputPeak))}%`}</strong></div><div className="health"><label>Output peak</label><strong>{peakPercent(telemetry.outputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.outputPeak))}%`}</strong></div></div>
       </section>
       </section>
    </main>
    <CalibrationDialog route={activeRoute + 1} record={showCalibration ? activeCalibration : undefined} live={activeSpectrum} onClose={() => setShowCalibration(false)} />
  </div>;
}

function App() { return <QueryClientProvider client={queryClient}><TooltipProvider><ErrorBoundary><Home /></ErrorBoundary><Toaster /></TooltipProvider></QueryClientProvider>; }
export default App;