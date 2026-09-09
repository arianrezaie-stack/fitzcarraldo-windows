import { useEffect, useMemo, useState, type ReactNode } from 'react';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { Activity, AlertTriangle, AudioLines, CircleHelp, Gauge, LockKeyhole, Mic2, Power, Radio, SlidersHorizontal, Sparkles, Volume2, Waves, Zap } from 'lucide-react';
import { ErrorBoundary } from '@/components/error-boundary';
import { Toaster } from '@/components/ui/toaster';
import { TooltipProvider } from '@/components/ui/tooltip';

const queryClient = new QueryClient();
const frequencyPosition = (frequency: number) => (Math.log10(frequency / 20) / Math.log10(20000 / 20)) * 100;
const spectrumTicks = [{ frequency: 20, label: '20 Hz' }, { frequency: 500, label: '500 Hz' }, { frequency: 2000, label: '2 kHz' }, { frequency: 8000, label: '8 kHz' }, { frequency: 20000, label: '20 kHz' }];

type Device = { deviceType: string; name: string; direction: 'input' | 'output' };
type EngineStatus = { state: string; reason?: string };
type Notch = { frequency: number; depthDb: number; q: number };
type Telemetry = { running?: boolean; sampleRate?: number; bufferSize?: number; callbackCpu?: number; xruns?: number; inputPeak?: number; outputPeak?: number; protectionEnabled?: boolean; preset?: 'speech' | 'music'; calibrating?: boolean; calibrated?: boolean; calibratedRoutes?: boolean[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumCutDb?: number };
type AudioState = { phase?: string; running?: boolean; sampleRate?: number; bufferSize?: number };
type Route = { id: number; enabled: boolean };

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

function Home() {
  const bridge = typeof window !== 'undefined' ? window.werfeedDesktop?.engine : undefined;
  const [engineStatus, setEngineStatus] = useState<EngineStatus>(bridge ? { state: 'connecting' } : { state: 'unavailable', reason: 'Electron preload bridge is not present.' });
  const [devices, setDevices] = useState<Device[]>([]);
  const [audioState, setAudioState] = useState<AudioState>({});
  const [telemetry, setTelemetry] = useState<Telemetry>({});
  const [routes, setRoutes] = useState<Route[]>([{ id: 1, enabled: true }, { id: 2, enabled: false }, { id: 3, enabled: false }, { id: 4, enabled: false }]);
  const [selection, setSelection] = useState('');
  const [buffer, setBuffer] = useState('128');
  const [message, setMessage] = useState('');
  const [pendingStart, setPendingStart] = useState(false);
  const [preset, setPreset] = useState<'speech' | 'music'>('speech');
  const [calibrationRoute, setCalibrationRoute] = useState(0);

  const pairs = useMemo(() => {
    const inputs = devices.filter((device) => device.direction === 'input');
    const outputs = devices.filter((device) => device.direction === 'output');
    return inputs.flatMap((input) => outputs.filter((output) => output.deviceType === input.deviceType).map((output) => ({ input, output, key: `${input.deviceType}\u0000${input.name}\u0000${output.name}` })))
      .sort((a, b) => Number(b.input.deviceType.toLowerCase().includes('exclusive')) - Number(a.input.deviceType.toLowerCase().includes('exclusive')));
  }, [devices]);
  const selectedPair = pairs.find((pair) => pair.key === selection);
  const nativeReady = !!bridge && engineStatus.state === 'running';
  const audioRunning = audioState.running ?? telemetry.running ?? false;
  const rate = telemetry.sampleRate ?? audioState.sampleRate;
  const actualBuffer = telemetry.bufferSize ?? audioState.bufferSize;
  const latency = rate && actualBuffer ? (actualBuffer * 2 / rate) * 1000 : undefined;
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
      if (payload.type === 'calibration') showToast(`Calibration saved · ${Number(payload.delayMs ?? 0).toFixed(1)} ms measured delay`);
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

  useEffect(() => { if (!selection && pairs[0]) setSelection(pairs[0].key); }, [pairs, selection]);

  const configureAndStart = () => {
    if (!bridge || !selectedPair || !nativeReady) return;
    const enabled = routes.filter((route) => route.enabled);
    if (!enabled.length) { showToast('Enable at least one mono route.'); return; }
    const channels = Math.max(...enabled.map((route) => route.id));
    setPendingStart(true);
    void bridge.command('configure', { deviceType: selectedPair.input.deviceType, inputDevice: selectedPair.input.name, outputDevice: selectedPair.output.name, sampleRate: 48000, bufferSize: Number(buffer), inputChannels: channels, outputChannels: channels, routes: enabled.map((route) => ({ input: route.id - 1, output: route.id - 1 })) }).catch((error: unknown) => { setPendingStart(false); showToast(error instanceof Error ? error.message : 'Unable to configure native audio'); });
  };
  const stop = () => { if (bridge && nativeReady) void bridge.command('stop').catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to stop native audio')); };
  const setProtection = (enabled: boolean, nextPreset = preset) => {
    if (!bridge || !nativeReady) return;
    setPreset(nextPreset);
    void bridge.command('set_protection', { enabled, preset: nextPreset }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change protection'));
  };
  const calibrate = () => {
    if (!bridge || !nativeReady || !audioRunning || telemetry.calibrating) return;
    const accepted = window.confirm('Calibration emits an audible impulse and sweep. Set speaker gain low, clear the room near the loudspeaker, and keep a physical mute ready. Start calibration?');
    if (accepted) void bridge.command('start_calibration', { route: calibrationRoute, level: 0.06 }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to start calibration'));
  };
  const value = (number: number | undefined, digits = 1) => number === undefined ? '—' : number.toFixed(digits);
  const peakPercent = (peak: number | undefined) => peak === undefined ? undefined : peak * 100;

  return <div className="app-shell">
    {message && <div className="toast" role="status"><AlertTriangle size={14} /> {message}</div>}
    <header className="app-header"><div className="brand-lockup"><div className="brand-mark"><AudioLines size={20} /></div><div><div className="eyebrow">Werfeed / adaptive feedback control</div><h1 className="brand-title">Werfeed Herzback <span>· acoustic protection</span></h1></div></div><div className="header-meta"><Badge tone={nativeReady ? 'green' : 'red'}><span className="route-dot" /> {bridge ? `Engine ${engineStatus.state}` : 'Native engine unavailable'}</Badge><span className="session">{engineStatus.reason ?? 'Native engine status'}</span></div></header>
    <main className="main-content">
      <section className="hero-panel"><div className="hero-grid"><div><div className="status-row"><Badge tone={audioRunning ? 'green' : 'red'}>{audioRunning ? <><Radio size={12} /> routing running</> : <><Power size={12} /> routing stopped</>}</Badge><span className="route-label">{telemetry.calibrating ? 'Calibration sweep in progress' : telemetry.protectionEnabled ? `${telemetry.preset ?? preset} protection active` : 'Protection bypassed'}</span></div><div className="hero-copy"><div><h2 className="hero-title">Measured acoustic protection</h2><p className="hero-description">Calibrate the loudspeaker loop, then use bounded adaptive notch filters to control persistent feedback while preserving speech or music.</p></div><div className="hero-actions"><button type="button" className={`action-button ${audioRunning ? 'off' : ''}`} disabled={!nativeReady || (!audioRunning && !selectedPair)} onClick={audioRunning ? stop : configureAndStart}>{audioRunning ? <><Power size={15} /> Stop</> : <><Zap size={15} /> Configure &amp; Start</>}</button></div></div>
        <div className="metrics">{[{ label: 'Callback CPU', value: value(telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100), unit: '%', icon: <Activity size={14} />, level: telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100 }, { label: 'Round-trip buffer', value: value(latency, 2), unit: 'ms est.', icon: <Gauge size={14} />, level: undefined }, { label: 'XRuns', value: telemetry.xruns === undefined ? '—' : String(telemetry.xruns), unit: 'local est.', icon: <Volume2 size={14} />, level: undefined }].map((metric) => <div className="metric" key={metric.label}><div className="metric-head"><span>{metric.label}</span>{metric.icon}</div><div className="metric-value">{metric.value} <small>{metric.unit}</small></div><Meter level={metric.level} /></div>)}</div></div>
        <aside className="guard-card"><div className="guard-card-header"><span className="flex-row"><Sparkles size={14} /> protection</span><Badge tone={telemetry.protectionEnabled ? 'green' : 'amber'}>{telemetry.protectionEnabled ? 'armed' : 'bypassed'}</Badge></div><div className="guard-orbit"><div><strong>{telemetry.activeNotches ?? 0}</strong><span>active cuts</span></div></div><div className="guard-summary"><strong>{telemetry.calibrated ? 'CALIBRATED BASELINE' : 'UNCALIBRATED BASELINE'}</strong><p>Maximum live cut {value(telemetry.maximumCutDb)} dB.<br />Filters are limited to six click-free notches.</p></div></aside></div></section>
      <section className="content-grid"><div className="panel"><div className="panel-heading"><div><div className="section-kicker"><SlidersHorizontal size={14} /> mono routing</div><h3 className="section-title">Enabled routes</h3><p className="section-note">Routes map same-numbered input and output channels when configured.</p></div><div className="mode-switch"><button type="button" className={preset === 'speech' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(telemetry.protectionEnabled === true, 'speech')}><Mic2 size={13} /> Speech</button><button type="button" className={preset === 'music' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(telemetry.protectionEnabled === true, 'music')}><Waves size={13} /> Music</button></div></div><div className="route-grid">{routes.map((route) => { const enabledIndex = routes.filter((item) => item.enabled).findIndex((item) => item.id === route.id); return <div className={`route-card ${route.enabled ? 'selected' : ''}`} key={route.id}><button type="button" className="route-card-top" disabled={audioRunning || !nativeReady} onClick={() => setRoutes((items) => items.map((item) => item.id === route.id ? { ...item, enabled: !item.enabled } : item))}><span className={`route-dot ${route.enabled ? 'locked' : ''}`} /><span className="channel">ROUTE {route.id}</span></button><div className="route-name">Mono channel {route.id}</div><div className="route-meta"><span>input {route.id} → output {route.id}</span></div><div className="route-card-bottom"><span>{route.enabled ? 'enabled' : 'disabled'}</span><span>{enabledIndex >= 0 && telemetry.calibratedRoutes?.[enabledIndex] ? 'baseline saved' : 'needs calibration'}</span></div></div>; })}</div><div className="curve-section"><div className="curve-toolbar"><div className="curve-name"><Radio size={15} /> Live analyzer / adaptive cuts</div></div><Spectrum values={telemetry.spectrumDb} notches={telemetry.notches} /></div></div>
      <div className="side-stack"><div className="panel"><div className="panel-heading"><div><h3 className="section-title">Live EQ moves</h3><p className="section-note">Baseline-relative tonal detections with hysteresis.</p></div><span className="slot-count">{telemetry.notches?.length ?? 0} / 6</span></div>{telemetry.notches?.length ? telemetry.notches.map((notch) => <div className="cut-item" key={`${notch.frequency}-${notch.q}`}><div className="cut-main"><span className="cut-frequency"><i className="cut-dot" />{notch.frequency.toFixed(0)} Hz</span><span className="cut-depth">{notch.depthDb.toFixed(1)} dB</span></div><div className="cut-meta"><span>Q {notch.q.toFixed(1)}</span><strong>tracking</strong></div></div>) : <div className="section-note">No active feedback cuts.</div>}<div className="armed-line"><Sparkles size={13} /> Auto-protect {telemetry.protectionEnabled ? 'armed' : 'bypassed'}</div></div>
      <div className="panel"><div className="section-kicker"><LockKeyhole size={14} /> WASAPI device &amp; engine</div><label className="device-select"><span><span className="device-name">{selectedPair ? `${selectedPair.input.name} → ${selectedPair.output.name}` : 'No compatible WASAPI input/output pair'}</span><span className="device-detail">{selectedPair?.input.deviceType ?? 'Awaiting native WASAPI device records'} · requested 48 kHz</span></span><select value={selection} disabled={!nativeReady || audioRunning || !pairs.length} onChange={(event) => setSelection(event.target.value)} aria-label="Compatible WASAPI input and output device">{!pairs.length && <option value="">No compatible WASAPI devices</option>}{pairs.map((pair) => <option key={pair.key} value={pair.key}>{pair.input.deviceType.toLowerCase().includes('exclusive') ? 'Exclusive · ' : 'Shared · '}{pair.input.name} → {pair.output.name}</option>)}</select></label><div className="detail-row"><span>Driver mode</span><strong>{selectedPair ? (selectedPair.input.deviceType.toLowerCase().includes('exclusive') ? 'WASAPI Exclusive' : 'WASAPI Shared') : '—'}</strong></div><div className="detail-row"><span>Requested buffer</span><select value={buffer} disabled={!nativeReady || audioRunning} onChange={(event) => setBuffer(event.target.value)}><option value="64">64 samples</option><option value="128">128 samples</option><option value="256">256 samples</option></select></div><div className="detail-row"><span>Engine state</span><strong>{audioState.phase ?? engineStatus.state}</strong></div><div className="health-grid"><div className="health"><label>Sample rate</label><strong>{rate === undefined ? '—' : `${rate} Hz`}</strong></div><div className="health"><label>Buffer</label><strong>{actualBuffer === undefined ? '—' : `${actualBuffer} smp`}</strong></div><div className="health"><label>Input peak</label><strong>{peakPercent(telemetry.inputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.inputPeak))}%`}</strong></div><div className="health"><label>Output peak</label><strong>{peakPercent(telemetry.outputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.outputPeak))}%`}</strong></div></div><p className="section-note">Exclusive mode is preferred for low latency. Shared mode is the compatibility fallback. Use one physical interface for both input and output to avoid clock drift.</p></div></div></section>
      <footer className="footer-bar"><div className="footer-note"><AlertTriangle size={14} /> {bridge ? 'Calibration is audible. Keep gain low and a physical mute within reach.' : 'Native engine unavailable: this web preview has no Electron preload bridge.'}</div><div className="footer-actions"><select className="calibration-route" value={calibrationRoute} disabled={!nativeReady || !audioRunning || telemetry.calibrating} onChange={(event) => setCalibrationRoute(Number(event.target.value))} aria-label="Calibration route">{routes.filter((route) => route.enabled).map((route, index) => <option key={route.id} value={index}>Route {route.id}</option>)}</select><button type="button" className="plain-button" disabled={!nativeReady || !audioRunning || telemetry.calibrating} onClick={calibrate}><CircleHelp size={14} /> {telemetry.calibrating ? 'Calibrating…' : 'Run calibration'}</button><button type="button" className="plain-button footer-bypass" disabled={!nativeReady} onClick={() => setProtection(!telemetry.protectionEnabled)}><Power size={13} /> {telemetry.protectionEnabled ? 'Bypass suppression' : 'Arm suppression'}</button></div></footer>
    </main></div>;
}

function App() { return <QueryClientProvider client={queryClient}><TooltipProvider><ErrorBoundary><Home /></ErrorBoundary><Toaster /></TooltipProvider></QueryClientProvider>; }
export default App;