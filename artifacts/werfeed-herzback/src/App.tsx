import { useEffect, useMemo, useState, type ReactNode } from 'react';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { Activity, AlertTriangle, AudioLines, BarChart3, CircleHelp, Gauge, LockKeyhole, Mic2, Power, Radio, SlidersHorizontal, Sparkles, Volume2, Waves, X, Zap } from 'lucide-react';
import { ErrorBoundary } from '@/components/error-boundary';
import { Toaster } from '@/components/ui/toaster';
import { TooltipProvider } from '@/components/ui/tooltip';

const queryClient = new QueryClient();
const frequencyPosition = (frequency: number) => (Math.log10(frequency / 20) / Math.log10(20000 / 20)) * 100;
const spectrumTicks = [{ frequency: 20, label: '20 Hz' }, { frequency: 500, label: '500 Hz' }, { frequency: 2000, label: '2 kHz' }, { frequency: 8000, label: '8 kHz' }, { frequency: 20000, label: '20 kHz' }];

type Device = { deviceType: string; name: string; direction: 'input' | 'output' };
type EngineStatus = { state: string; reason?: string };
type Notch = { frequency: number; depthDb: number; q: number };
type RouteSnapshot = { route: number; enabled?: boolean; suppression?: number; calibrated?: boolean; delayMs?: number; calibrationResponseDb?: number[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumCutDb?: number };
type Telemetry = { running?: boolean; sampleRate?: number; bufferSize?: number; callbackCpu?: number; xruns?: number; inputPeak?: number; outputPeak?: number; protectionEnabled?: boolean; preset?: 'speech' | 'music'; calibrating?: boolean; calibrated?: boolean; calibratedRoutes?: boolean[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumCutDb?: number; routeTelemetry?: RouteSnapshot[] };
type AudioState = { phase?: string; running?: boolean; sampleRate?: number; bufferSize?: number };
type Route = { id: number; enabled: boolean; suppression: number };
type CalibrationRecord = { delayMs?: number; responseDb: number[] };
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
    <div className="spectrum-grid" /><div className="spectrum-label">{points ? 'Live 96-bin detector · baseline-relative protection' : 'Awaiting native analyzer data'}</div>
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

function Home() {
  const bridge = typeof window !== 'undefined' ? window.werfeedDesktop?.engine : undefined;
  const [engineStatus, setEngineStatus] = useState<EngineStatus>(bridge ? { state: 'connecting' } : { state: 'unavailable', reason: 'Electron preload bridge is not present.' });
  const [devices, setDevices] = useState<Device[]>([]);
  const [audioState, setAudioState] = useState<AudioState>({});
  const [telemetry, setTelemetry] = useState<Telemetry>({});
  const [routes, setRoutes] = useState<Route[]>([
    { id: 1, enabled: true, suppression: 0.75 },
    { id: 2, enabled: true, suppression: 0.75 },
    { id: 3, enabled: true, suppression: 0.75 },
    { id: 4, enabled: true, suppression: 0.75 },
  ]);
  const [selection, setSelection] = useState('');
  const [buffer, setBuffer] = useState('128');
  const [message, setMessage] = useState('');
  const [pendingStart, setPendingStart] = useState(false);
  const [preset, setPreset] = useState<'speech' | 'music'>('speech');
  const [activeRoute, setActiveRoute] = useState(0);
  const [calibrations, setCalibrations] = useState<Record<number, CalibrationRecord>>({});
  const [showCalibration, setShowCalibration] = useState(false);

  const pairs = useMemo(() => {
    const inputs = devices.filter((device) => device.direction === 'input');
    const outputs = devices.filter((device) => device.direction === 'output');
    return inputs.flatMap((input) => outputs.filter((output) => output.deviceType === input.deviceType).map((output) => ({ input, output, key: `${input.deviceType}\u0000${input.name}\u0000${output.name}` })))
      .sort((a, b) => backendPriority(a.input.deviceType) - backendPriority(b.input.deviceType));
  }, [devices]);
  const selectedPair = pairs.find((pair) => pair.key === selection);
  const nativeReady = !!bridge && engineStatus.state === 'running';
  const audioRunning = audioState.running ?? telemetry.running ?? false;
  const rate = telemetry.sampleRate ?? audioState.sampleRate;
  const actualBuffer = telemetry.bufferSize ?? audioState.bufferSize;
  const latency = rate && actualBuffer ? (actualBuffer * 2 / rate) * 1000 : undefined;
  const activeRouteState = routes[activeRoute] ?? routes[0];
  const activeRouteTelemetry = telemetry.routeTelemetry?.find((route) => route.route === activeRoute + 1);
  const activeCalibration = calibrations[activeRoute + 1] ?? (activeRouteTelemetry?.calibrationResponseDb ? {
    delayMs: activeRouteTelemetry.delayMs,
    responseDb: activeRouteTelemetry.calibrationResponseDb,
  } : undefined);
  const activeSpectrum = activeRouteTelemetry?.spectrumDb ?? telemetry.spectrumDb;
  const activeNotches = activeRouteTelemetry?.notches ?? telemetry.notches;
  const showToast = (text: string) => { setMessage(text); window.setTimeout(() => setMessage(''), 2800); };

  useEffect(() => {
    if (!bridge) return;
    const handleEvent = (event: unknown) => {
      if (!event || typeof event !== 'object' || !('type' in event)) return;
      const payload = event as Record<string, unknown>;
      if (payload.type === 'hello') showToast(`Native engine ${String(payload.engineVersion ?? '')} connected`);
      if (payload.type === 'devices' && Array.isArray(payload.devices)) {
        const records = payload.devices.filter((item): item is Device => !!item && typeof item === 'object' && typeof (item as Device).deviceType === 'string' && typeof (item as Device).name === 'string' && ((item as Device).direction === 'input' || (item as Device).direction === 'output'));
        setDevices(records);
      }
      if (payload.type === 'state') {
        const state = payload as AudioState & { type: string };
        setAudioState({ phase: state.phase, running: state.running, sampleRate: state.sampleRate, bufferSize: state.bufferSize });
        if (typeof state.running === 'boolean') setTelemetry((current) => ({ ...current, running: state.running }));
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
        showToast(`Route ${route || '—'} calibration saved · ${Number(payload.delayMs ?? 0).toFixed(1)} ms measured delay`);
      }
      if (payload.type === 'error') showToast(String(payload.message ?? 'Native engine error'));
    };
    const removeStatus = bridge.onStatus((status) => setEngineStatus(status));
    const removeEvent = bridge.onEvent(handleEvent);
    void bridge.getStatus().then(setEngineStatus).catch((error: unknown) => setEngineStatus({ state: 'unavailable', reason: error instanceof Error ? error.message : 'Unable to read native engine status' }));
    return () => { removeStatus(); removeEvent(); };
  }, [bridge, pendingStart]);

  useEffect(() => {
    if (!bridge || engineStatus.state !== 'running') return;
    void bridge.command('list_devices').catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to list audio devices'));
  }, [bridge, engineStatus.state]);

  useEffect(() => {
    if (!bridge?.validation || !devices.length) return;
    bridge.validation.reportDevices(devices, pairs.map(({ input, output }) => ({
      deviceType: input.deviceType,
      input: input.name,
      output: output.name,
    })));
  }, [bridge, devices, pairs]);

  useEffect(() => { if (!selection && pairs[0]) setSelection(pairs[0].key); }, [pairs, selection]);

  const configureAndStart = () => {
    if (!bridge || !selectedPair || !nativeReady) return;
    const enabled = routes.filter((route) => route.enabled);
    if (!enabled.length) { showToast('Enable at least one mono route.'); return; }
    const channels = Math.max(...enabled.map((route) => route.id));
    setPendingStart(true);
    void bridge.command('configure', {
      deviceType: selectedPair.input.deviceType,
      inputDevice: selectedPair.input.name,
      outputDevice: selectedPair.output.name,
      sampleRate: 48000,
      bufferSize: Number(buffer),
      inputChannels: channels,
      outputChannels: channels,
      // Keep every route index stable. Disabled routes remain present as -1
      // entries so calibration and telemetry never shift to another route.
      routes: routes.map((route) => ({
        input: route.id - 1,
        output: route.id - 1,
        enabled: route.enabled,
        suppression: route.suppression,
      })),
    }).catch((error: unknown) => { setPendingStart(false); showToast(error instanceof Error ? error.message : 'Unable to configure native audio'); });
  };
  const stop = () => { if (bridge && nativeReady) void bridge.command('stop').catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to stop native audio')); };
  const setProtection = (enabled: boolean, nextPreset = preset) => {
    if (!bridge || !nativeReady) return;
    setPreset(nextPreset);
    void bridge.command('set_protection', { enabled, preset: nextPreset }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change protection'));
  };
  const setSuppression = (value: number) => {
    const nextValue = Math.max(0, Math.min(1, value));
    setRoutes((items) => items.map((route) => route.id === activeRoute + 1 ? { ...route, suppression: nextValue } : route));
    if (!bridge || !nativeReady || !audioRunning) return;
    void bridge.command('set_protection', { route: activeRoute, suppression: nextValue }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change route suppression'));
  };
  const calibrate = () => {
    if (!bridge || !nativeReady || !audioRunning || telemetry.calibrating) return;
    const accepted = window.confirm('Calibration emits an audible impulse and sweep. Set speaker gain low, clear the room near the loudspeaker, and keep a physical mute ready. Start calibration?');
    if (accepted) void bridge.command('start_calibration', { route: activeRoute, level: 0.06 }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to start calibration'));
  };
  const value = (number: number | undefined, digits = 1) => number === undefined ? '—' : number.toFixed(digits);
  const peakPercent = (peak: number | undefined) => peak === undefined ? undefined : peak * 100;

  return <div className="app-shell">
    {message && <div className="toast" role="status"><AlertTriangle size={14} /> {message}</div>}
    <header className="app-header"><div className="brand-lockup"><div className="brand-mark"><AudioLines size={20} /></div><div><div className="eyebrow">Werfeed / adaptive feedback control</div><h1 className="brand-title">Werfeed Herzback <span>· acoustic protection</span></h1></div></div><div className="header-meta"><Badge tone={nativeReady ? 'green' : 'red'}><span className="route-dot" /> {bridge ? `Engine ${engineStatus.state}` : 'Native engine unavailable'}</Badge><span className="session">{engineStatus.reason ?? 'Native engine status'}</span></div></header>
    <main className="main-content">
      <section className="hero-panel"><div className="hero-grid"><div><div className="status-row"><Badge tone={audioRunning ? 'green' : 'red'}>{audioRunning ? <><Radio size={12} /> routing running</> : <><Power size={12} /> routing stopped</>}</Badge><span className="route-label">{telemetry.calibrating ? 'Calibration sweep in progress' : telemetry.protectionEnabled ? `${telemetry.preset ?? preset} protection active` : 'Protection bypassed'}</span></div><div className="hero-copy"><div><h2 className="hero-title">Measured acoustic protection</h2><p className="hero-description">Calibrate the loudspeaker loop, then use bounded adaptive notch filters to control persistent feedback while preserving speech or music.</p></div><div className="hero-actions"><button type="button" className={`action-button ${audioRunning ? 'off' : ''}`} disabled={!nativeReady || (!audioRunning && !selectedPair)} onClick={audioRunning ? stop : configureAndStart}>{audioRunning ? <><Power size={15} /> Stop</> : <><Zap size={15} /> Configure &amp; Start</>}</button></div></div>
        <div className="metrics">{[{ label: 'Callback CPU', value: value(telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100), unit: '%', icon: <Activity size={14} />, level: telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100 }, { label: 'Round-trip buffer', value: value(latency, 2), unit: 'ms est.', icon: <Gauge size={14} />, level: undefined }, { label: 'XRuns', value: telemetry.xruns === undefined ? '—' : String(telemetry.xruns), unit: 'local est.', icon: <Volume2 size={14} />, level: undefined }].map((metric) => <div className="metric" key={metric.label}><div className="metric-head"><span>{metric.label}</span>{metric.icon}</div><div className="metric-value">{metric.value} <small>{metric.unit}</small></div><Meter level={metric.level} /></div>)}</div></div>
         <aside className="guard-card"><div className="guard-card-header"><span className="flex-row"><Sparkles size={14} /> route {activeRoute + 1} protection</span><Badge tone={telemetry.protectionEnabled ? 'green' : 'amber'}>{telemetry.protectionEnabled ? 'armed' : 'bypassed'}</Badge></div><div className="guard-orbit"><div><strong>{activeRouteTelemetry?.activeNotches ?? telemetry.activeNotches ?? 0}</strong><span>active cuts</span></div></div><div className="guard-summary"><strong>{activeRouteTelemetry?.calibrated || activeCalibration ? 'CALIBRATED BASELINE' : 'UNCALIBRATED BASELINE'}</strong><p>Maximum live cut {value(activeRouteTelemetry?.maximumCutDb ?? telemetry.maximumCutDb)} dB.<br />All four route processors remain live simultaneously.</p></div></aside></div></section>
        <section className="content-grid"><div className="panel"><div className="panel-heading"><div><div className="section-kicker"><SlidersHorizontal size={14} /> mono routing</div><h3 className="section-title">Four simultaneous routes</h3><p className="section-note">Select a route to inspect its analyzer, calibration, and suppression fader. Every armed route processes independently.</p></div><div className="mode-switch"><button type="button" className={preset === 'speech' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(telemetry.protectionEnabled === true, 'speech')}><Mic2 size={13} /> Speech</button><button type="button" className={preset === 'music' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(telemetry.protectionEnabled === true, 'music')}><Waves size={13} /> Music</button></div></div><div className="route-grid">{routes.map((route) => { const routeInfo = telemetry.routeTelemetry?.find((item) => item.route === route.id); const calibrated = routeInfo?.calibrated || Boolean(calibrations[route.id]); return <div className={`route-card ${route.id === activeRoute + 1 ? 'selected' : ''} ${route.enabled ? '' : 'muted'}`} key={route.id}><button type="button" className="route-card-top" disabled={!nativeReady} onClick={() => setActiveRoute(route.id - 1)} aria-pressed={route.id === activeRoute + 1}><span className={`route-dot ${route.enabled ? 'locked' : ''}`} /><span className="channel">ROUTE {route.id}</span><span className="route-select-label">{route.id === activeRoute + 1 ? 'viewing' : 'select'}</span></button><div className="route-name">Input {route.id} → Output {route.id}</div><div className="route-meta"><span>{route.enabled ? 'live mono path' : 'standby'}</span><label className="route-arm"><input type="checkbox" checked={route.enabled} disabled={audioRunning || !nativeReady} onChange={() => setRoutes((items) => items.map((item) => item.id === route.id ? { ...item, enabled: !item.enabled } : item))} /><span>arm</span></label></div><div className="route-card-bottom"><span>{route.enabled ? 'processing' : 'disabled'}</span><span>{calibrated ? 'baseline saved' : 'needs calibration'}</span></div></div>; })}</div><div className="route-fader"><div className="fader-copy"><div className="curve-name"><SlidersHorizontal size={14} /> Route {activeRoute + 1} suppression amount</div><p className="section-note">Higher values reach deeper, faster narrow-band cuts. Speech stays narrow to protect fundamentals.</p></div><div className="fader-control"><div className="fader-scale"><span>light</span><span>deep</span></div><input className="suppression-slider" type="range" min="0" max="100" step="1" value={Math.round((activeRouteState?.suppression ?? 0.75) * 100)} disabled={!nativeReady} onChange={(event) => setSuppression(Number(event.target.value) / 100)} aria-label={`Route ${activeRoute + 1} suppression amount`} style={{ background: `linear-gradient(90deg, #d19a63 0%, #d19a63 ${Math.round((activeRouteState?.suppression ?? 0.75) * 100)}%, #4a3025 ${Math.round((activeRouteState?.suppression ?? 0.75) * 100)}%, #4a3025 100%)` }} /><strong>{Math.round((activeRouteState?.suppression ?? 0.75) * 100)}%</strong></div></div><div className="curve-section"><div className="curve-toolbar"><div className="curve-name"><Radio size={15} /> Route {activeRoute + 1} live analyzer / adaptive cuts</div><span className="slot-count">{activeNotches?.length ?? 0} / 6 cuts</span></div><Spectrum values={activeSpectrum} notches={activeNotches} /></div></div>
       <div className="side-stack"><div className="panel"><div className="panel-heading"><div><h3 className="section-title">Route {activeRoute + 1} live EQ moves</h3><p className="section-note">Calibration-weighted tonal detections with faster persistence and narrow Q.</p></div><span className="slot-count">{activeNotches?.length ?? 0} / 6</span></div>{activeNotches?.length ? activeNotches.map((notch) => <div className="cut-item" key={`${notch.frequency}-${notch.q}`}><div className="cut-main"><span className="cut-frequency"><i className="cut-dot" />{notch.frequency.toFixed(0)} Hz</span><span className="cut-depth">{notch.depthDb.toFixed(1)} dB</span></div><div className="cut-meta"><span>Q {notch.q.toFixed(1)}</span><strong>tracking</strong></div></div>) : <div className="section-note">No active feedback cuts on Route {activeRoute + 1}.</div>}<div className="armed-line"><Sparkles size={13} /> Auto-protect {telemetry.protectionEnabled ? 'armed on all routes' : 'bypassed'}</div></div>
      <div className="panel"><div className="section-kicker"><LockKeyhole size={14} /> audio backend &amp; engine</div><label className="device-select"><span><span className="device-name">{selectedPair ? `${selectedPair.input.name} → ${selectedPair.output.name}` : 'No compatible input/output pair'}</span><span className="device-detail">{selectedPair ? backendLabel(selectedPair.input.deviceType) : 'Awaiting native audio device records'} · requested 48 kHz</span></span><select value={selection} disabled={!nativeReady || audioRunning || !pairs.length} onChange={(event) => setSelection(event.target.value)} aria-label="Compatible input/output device">{!pairs.length && <option value="">No compatible audio devices</option>}{pairs.map((pair) => <option key={pair.key} value={pair.key}>{backendLabel(pair.input.deviceType)} · {pair.input.name} → {pair.output.name}</option>)}</select></label><div className="detail-row"><span>Selected backend</span><strong>{selectedPair ? backendLabel(selectedPair.input.deviceType) : '—'}</strong></div><div className="detail-row"><span>Requested buffer</span><select value={buffer} disabled={!nativeReady || audioRunning} onChange={(event) => setBuffer(event.target.value)}><option value="64">64 samples</option><option value="128">128 samples</option><option value="256">256 samples</option></select></div><div className="detail-row"><span>Engine state</span><strong>{audioState.phase ?? engineStatus.state}</strong></div><div className="health-grid"><div className="health"><label>Sample rate</label><strong>{rate === undefined ? '—' : `${rate} Hz`}</strong></div><div className="health"><label>Buffer</label><strong>{actualBuffer === undefined ? '—' : `${actualBuffer} smp`}</strong></div><div className="health"><label>Input peak</label><strong>{peakPercent(telemetry.inputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.inputPeak))}%`}</strong></div><div className="health"><label>Output peak</label><strong>{peakPercent(telemetry.outputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.outputPeak))}%`}</strong></div></div><p className="section-note">Each row uses the backend shown. WASAPI Exclusive is preferred for low latency; Shared is the compatibility fallback. DirectSound and ASIO use their own driver paths.</p></div></div></section>
       <footer className="footer-bar"><div className="footer-note"><AlertTriangle size={14} /> {bridge ? 'Calibration is audible. Keep gain low and a physical mute within reach.' : 'Native engine unavailable: this web preview has no Electron preload bridge.'}</div><div className="footer-actions"><select className="calibration-route" value={activeRoute} disabled={!nativeReady || !audioRunning || telemetry.calibrating} onChange={(event) => setActiveRoute(Number(event.target.value))} aria-label="Calibration route">{routes.map((route) => <option key={route.id} value={route.id - 1}>Route {route.id}</option>)}</select>{activeCalibration && <button type="button" className="plain-button" onClick={() => setShowCalibration(true)}><BarChart3 size={14} /> View trace</button>}<button type="button" className="plain-button" disabled={!nativeReady || !audioRunning || telemetry.calibrating || !activeRouteState?.enabled} onClick={calibrate}><CircleHelp size={14} /> {telemetry.calibrating ? 'Calibrating…' : `Calibrate Route ${activeRoute + 1}`}</button><button type="button" className="plain-button footer-bypass" disabled={!nativeReady} onClick={() => setProtection(!telemetry.protectionEnabled)}><Power size={13} /> {telemetry.protectionEnabled ? 'Bypass suppression' : 'Arm suppression'}</button></div></footer>
     </main><CalibrationDialog route={activeRoute + 1} record={showCalibration ? activeCalibration : undefined} live={activeSpectrum} onClose={() => setShowCalibration(false)} /></div>;
}

function App() { return <QueryClientProvider client={queryClient}><TooltipProvider><ErrorBoundary><Home /></ErrorBoundary><Toaster /></TooltipProvider></QueryClientProvider>; }
export default App;