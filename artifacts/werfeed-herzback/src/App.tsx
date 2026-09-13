import { useEffect, useMemo, useRef, useState, type CSSProperties, type ReactNode } from 'react';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { Activity, AlertTriangle, BarChart3, CircleHelp, Gauge, LockKeyhole, Mic2, MoreHorizontal, Power, Radio, RefreshCw, RotateCcw, SlidersHorizontal, Timer, TimerReset, Waves, X, Zap } from 'lucide-react';
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
type RouteSnapshot = { route: number; enabled?: boolean; depth?: number; sensitivity?: number; suppression?: number; timing?: number; latch?: number; calibrated?: boolean; delayMs?: number; calibrationResponseDb?: number[]; calibrationRawResponseDb?: number[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumAllowedNotches?: number; maximumCutDb?: number };
type Telemetry = { running?: boolean; sampleRate?: number; bufferSize?: number; callbackCpu?: number; xruns?: number; callbackDeadlineMisses?: number; driverXruns?: number; callbackExecutionMs?: number; callbackExecutionPeakMs?: number; callbackJitterMs?: number; callbackJitterPeakMs?: number; deviceClockDriftPpm?: number; deviceClockReady?: boolean; deviceClockAgeMs?: number; clockMeasurementSource?: string; inputPeak?: number; outputPeak?: number; protectionEnabled?: boolean; preset?: 'speech' | 'music'; calibrating?: boolean; calibrated?: boolean; calibratedRoutes?: boolean[]; spectrumDb?: number[]; notches?: Notch[]; activeNotches?: number; maximumCutDb?: number; routeTelemetry?: RouteSnapshot[] };
type AudioState = { phase?: string; running?: boolean; sampleRate?: number; bufferSize?: number };
type Route = { id: number; enabled: boolean; depth: number; sensitivity: number; timing: number; latch: number; pairKey: string; inputKey: string; outputKey: string; inputChannel: number; outputChannel: number };
type CalibrationRecord = { delayMs?: number; responseDb: number[]; rawResponseDb?: number[] };
type RecurringCutEvent = { frequency: number; timestamp: number; qualified: boolean };
type RecurringCutAlert = { route: number; frequencies: number[] };
const recurringCutWindowMs = 2_500;
const recurringCutDisplayMs = 5_000;
const recurringCutThreshold = 3;
const maximumSuggestedCuts = 3;
const sameCutFrequency = (left: number, right: number) => Math.abs(Math.log2(left / right)) < 0.08;
const maximumDepthDbForAmount = (amount: number) => {
  const clamped = Math.max(0, Math.min(1, amount));
  const legacyDepth = clamped <= 0.7 ? clamped : Math.min(1, 0.7 + (clamped - 0.7) * 3);
  const depthCore = Math.min(1, legacyDepth / 0.7);
  const depthExtension = Math.max(0, Math.min(1, (legacyDepth - 0.7) / 0.3));
  const depthExtra = Math.max(0, Math.min(1, (clamped - 0.8) / 0.2));
  return (14 * depthCore + 10 * depthExtension + 8 * depthExtra) * (1 + 0.2 * depthExtra);
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

const notchBandWidth = (frequency: number, q: number) => {
  const octaveSpan = Math.max(0.025, Math.min(0.16, 0.5 / Math.max(4, q)));
  const left = Math.max(20, frequency / Math.pow(2, octaveSpan));
  const right = Math.min(20000, frequency * Math.pow(2, octaveSpan));
  return Math.max(0.35, frequencyPosition(right) - frequencyPosition(left));
};

const clamp = (value: number, minimum: number, maximum: number) => Math.max(minimum, Math.min(maximum, value));

const legacySuppressionAmount = (amount: number) => {
  const clamped = clamp(amount, 0, 1);
  return clamped <= 0.7 ? clamped : Math.min(1, 0.7 + (clamped - 0.7) * 3);
};

const frequencyThresholdAdjustmentDb = (frequency: number, preset: 'speech' | 'music') => {
  if (preset === 'music') return 0;
  if (frequency <= 150) return 10;
  if (frequency < 500) {
    const position = Math.log(frequency / 150) / Math.log(500 / 150);
    return 10 * (1 - position);
  }
  if (frequency <= 1500) return 0;
  if (frequency < 8000) {
    const position = Math.log(frequency / 1500) / Math.log(8000 / 1500);
    return -18 * position;
  }
  return -18;
};

const calibrationBiasCurve = (response: number[]) => {
  if (response.length < 2 || response.some((value) => !Number.isFinite(value))) return [];
  const sorted = [...response].sort((left, right) => left - right);
  const median = sorted[Math.floor(sorted.length / 2)];
  const bias = Array.from({ length: response.length }, () => 0);
  response.forEach((value, index) => {
    const peakAboveMedian = Math.max(0, value - median - 2);
    const centerBias = Math.min(16, peakAboveMedian * 1.25);
    bias[index] = Math.max(bias[index], centerBias);
    if (index > 0) bias[index - 1] = Math.max(bias[index - 1], centerBias * 0.7);
    if (index + 1 < bias.length) bias[index + 1] = Math.max(bias[index + 1], centerBias * 0.7);
    if (index > 1) bias[index - 2] = Math.max(bias[index - 2], centerBias * 0.35);
    if (index + 2 < bias.length) bias[index + 2] = Math.max(bias[index + 2], centerBias * 0.35);
  });
  return bias;
};

const detectorHotspotSensitivityLift = (measuredPeakBias: number) => {
  const measuredExcess = Math.max(0, Math.max(0, measuredPeakBias) / 1.25 + 2);
  return clamp(measuredExcess / 16, 0.2, 0.5);
};

const detectorSensitivityAmount = (sensitivity: number, calibrationHotspot: boolean, measuredPeakBias: number) => {
  const clamped = clamp(sensitivity, 0, 1);
  return calibrationHotspot
    ? Math.min(1, clamped + detectorHotspotSensitivityLift(measuredPeakBias))
    : clamped;
};

const detectorCurveValues = (
  sensitivity: number,
  preset: 'speech' | 'music',
  calibrationResponse: number[],
) => {
  const biasCurve = calibrationBiasCurve(calibrationResponse);
  const pointCount = Math.max(2, calibrationResponse.length || 256);
  return Array.from({ length: pointCount }, (_, index) => {
    const position = index / (pointCount - 1);
    const frequency = 20 * Math.pow(1000, position);
    const measuredPeakBias = biasCurve[index] ?? 0;
    const calibrationHotspot = measuredPeakBias >= 1.5;
    const candidateSensitivity = detectorSensitivityAmount(
      sensitivity,
      calibrationHotspot,
      measuredPeakBias,
    );
    const candidateLegacyAmount = legacySuppressionAmount(candidateSensitivity);
    const candidateExtraSensitivity = clamp((candidateSensitivity - 0.8) / 0.2, 0, 1);
    const candidateSpeechCore = Math.min(1, candidateLegacyAmount / 0.7);
    const candidateSpeechExtension = clamp((candidateLegacyAmount - 0.7) / 0.3, 0, 1);
    const engageAboveBaseline = preset === 'speech'
      ? 9 - 5.5 * candidateSpeechCore - 3.5 * candidateSpeechExtension - 3 * candidateExtraSensitivity
      : 9 - 1.5 * candidateLegacyAmount - 3 * candidateExtraSensitivity;
    const frequencyAdjustment = frequencyThresholdAdjustmentDb(frequency, preset);
    const engageThreshold = Math.max(
      0.25,
      engageAboveBaseline + frequencyAdjustment
        - (calibrationHotspot ? Math.min(5, Math.max(0, measuredPeakBias) * 0.65) : 0),
    );
    const baseline = -55 - measuredPeakBias;
    const absoluteAmplitudeThreshold = -70 * candidateSensitivity + frequencyAdjustment;
    // A candidate must clear both the baseline-relative engage gate and the
    // absolute amplitude gate. Plot the higher of those two exact native
    // thresholds against the live absolute FFT spectrum.
    return Math.max(baseline + engageThreshold, absoluteAmplitudeThreshold);
  });
};

function Spectrum({
  values = [],
  notches = [],
  holdMs,
  releaseMs,
  sensitivity = 0.75,
  preset = 'speech',
  calibrationResponse = [],
}: {
  values?: number[];
  notches?: Notch[];
  holdMs?: number;
  releaseMs?: number;
  sensitivity?: number;
  preset?: 'speech' | 'music';
  calibrationResponse?: number[];
}) {
  const [hoverReadout, setHoverReadout] = useState<{ x: number; y: number; frequency: number } | null>(null);
  const visibleNotches = notches.filter((notch) =>
    Number.isFinite(notch.frequency) && notch.frequency > 0
      && Number.isFinite(notch.depthDb) && notch.depthDb < -0.5);
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
  const detectionCurvePoints = detectorCurveValues(sensitivity, preset, calibrationResponse).map((value, index, curve) => {
    const x = index / (curve.length - 1) * 100;
    const y = Math.max(0, Math.min(100, (0 - value) / 100 * 100));
    return `${x},${y}`;
  }).join(' ');
  return <div className="spectrum-stack">
    <div className="spectrum" data-testid="analyzer-spectrum" onMouseMove={updateHover} onMouseLeave={() => setHoverReadout(null)}>
      <div className="spectrum-grid" />
      <div className="spectrum-label">{points ? 'Live 256-bin detector · native detection floor shown' : 'Awaiting native analyzer data · detection floor shown'}</div>
      {detectionCurvePoints && <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Native detection floor and live spectrum"><title>Exact native detection floor, including sensitivity, frequency shaping, and calibration hotspot bias</title><polyline className="spectrum-detection-curve" points={detectionCurvePoints} /></svg>}
      {points && <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Live spectrum"><polyline className="spectrum-trace" points={points} /></svg>}
      <div className="spectrum-plot-overlay" aria-hidden="true">
        {visibleNotches.map((notch) => {
          const depthRatio = Math.max(0, Math.min(1, Math.abs(notch.depthDb) / 40));
          const width = notchBandWidth(notch.frequency, notch.q);
          return <div
            key={`${notch.frequency}-${notch.q}`}
            className="notch-band"
            style={{
              left: `${frequencyPosition(notch.frequency) - width / 2}%`,
              width: `${width}%`,
              height: `${18 + depthRatio * 68}%`,
            }}
            title={`${notch.frequency.toFixed(0)} Hz · ${Math.abs(notch.depthDb).toFixed(1)} dB cut · Q ${notch.q.toFixed(1)} · hold ${holdMs ?? 0} ms · release ${releaseMs ?? 0} ms`}
          >
            <i className="notch-marker" />
          </div>;
        })}
      </div>
      <div className="spectrum-legend"><span><i className="legend-line" /> live spectrum</span><span><i className="legend-detection" /> native detection floor</span><span><i className="legend-cut" /> {visibleNotches.length} adaptive cuts</span></div>
      <div className="spectrum-axis">{spectrumTicks.map((tick, index) => <span key={tick.frequency} className={index === 0 ? 'first' : index === spectrumTicks.length - 1 ? 'last' : ''} style={{ left: `${frequencyPosition(tick.frequency)}%` }}>{tick.label}</span>)}</div>
      {hoverReadout && <div className={`spectrum-hover-readout ${hoverReadout.x > 120 ? 'align-left' : ''}`} style={{ left: hoverReadout.x, top: hoverReadout.y }} aria-hidden="true">{hoverReadout.frequency.toLocaleString('en-US')} Hz</div>}
    </div>
    {visibleNotches.length > 0 && <div className="notch-inspector" aria-label="Adaptive notch estimates">
      <div className="notch-inspector-heading"><span>Active notch estimates</span><small>depth and Q are live · hold and release are route estimates</small></div>
      <div className="notch-inspector-grid">
        {visibleNotches.map((notch) => {
          const depthRatio = Math.max(0, Math.min(1, Math.abs(notch.depthDb) / 40));
          return <div className="notch-card" key={`readout-${notch.frequency}-${notch.q}`}>
            <div className="notch-card-heading"><strong>{notch.frequency.toLocaleString('en-US', { maximumFractionDigits: 0 })} Hz</strong><span>Q {notch.q.toFixed(1)}</span></div>
            <div className="notch-depth-track" aria-label={`${Math.abs(notch.depthDb).toFixed(1)} decibel cut`}><i style={{ width: `${depthRatio * 100}%` }} /></div>
            <div className="notch-card-metrics"><span>cut <strong>−{Math.abs(notch.depthDb).toFixed(1)} dB</strong></span><span>hold <strong>{holdMs ?? 0} ms</strong></span><span>release <strong>{releaseMs ?? 0} ms</strong></span></div>
          </div>;
        })}
      </div>
    </div>}
  </div>;
}

function TraceChart({ raw = [], normalized = [], live = [] }: { raw?: number[]; normalized?: number[]; live?: number[] }) {
  const toPoints = (values: number[]) => values.length > 1 ? values.map((value, index) => {
    const x = index / (values.length - 1) * 100;
    const bounded = Math.max(-48, Math.min(18, value));
    const y = 100 - ((bounded + 48) / 66) * 100;
    return `${x},${y}`;
  }).join(' ') : '';
  const rawPoints = toPoints(raw);
  const normalizedPoints = toPoints(normalized);
  const livePoints = toPoints(live);
  const flatPoints = toPoints(normalized.length > 1 ? normalized.map(() => 0) : []);
  return <div className="trace-chart">
    <div className="trace-grid" />
    <div className="trace-y-axis"><span>+18</span><span>0</span><span>-24</span><span>-48 dB</span></div>
    <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Calibration frequency response comparison">
      {flatPoints && <polyline className="trace-flat" points={flatPoints} />}
      {rawPoints && <polyline className="trace-raw" points={rawPoints} />}
      {normalizedPoints && <polyline className="trace-normalized" points={normalizedPoints} />}
      {livePoints && <polyline className="trace-live" points={livePoints} />}
    </svg>
    <div className="trace-axis">{spectrumTicks.map((tick, index) => <span key={tick.frequency} className={index === 0 ? 'first' : index === spectrumTicks.length - 1 ? 'last' : ''} style={{ left: `${frequencyPosition(tick.frequency)}%` }}>{tick.label}</span>)}</div>
    <div className="trace-legend"><span><i className="trace-key raw" /> original measured response</span><span><i className="trace-key normalized" /> normalized calibration response</span><span><i className="trace-key flat" /> flat reference · 0 dB</span><span><i className="trace-key live" /> current live spectrum</span></div>
  </div>;
}

function CalibrationDialog({ route, record, live, onClose }: { route: number; record?: CalibrationRecord; live?: number[]; onClose: () => void }) {
  if (!record) return null;
  return <div className="note-overlay" role="dialog" aria-modal="true" aria-labelledby="calibration-dialog-title">
    <div className="note-dialog calibration-dialog">
      <div className="dialog-heading"><div><div className="section-kicker"><BarChart3 size={14} /> route {route} measurement</div><h3 id="calibration-dialog-title">Calibration trace comparison</h3></div><button type="button" className="dialog-close" onClick={onClose} aria-label="Close calibration comparison"><X size={16} /></button></div>
      <p>The copper trace is the original measured room and loudspeaker response. The white trace is the same curve after its broadband offset is applied. The thin line marks the 0 dB flat reference; the pale dashed trace is the current route spectrum.</p>
      <TraceChart raw={record.rawResponseDb ?? record.responseDb} normalized={record.responseDb} live={live} />
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
    <div className="detail-row"><span>Engine state</span><strong>{restartingAudio ? 'restarting engine…' : audioRunning ? 'running · bypassed until armed' : nativeReady ? 'connected · waiting for mappings' : 'unavailable'}</strong></div>
    <button type="button" className="restart-audio-button" disabled={!restartAvailable || restartingAudio} onClick={onRestartAudio}><RefreshCw size={14} /> {restartingAudio ? 'Restarting engine…' : 'Restart engine'}</button>
    <p className="section-note">The native engine receives the exact device names and channel indices selected here. Standby routes send no audio but remain selectable and mappable. The engine restarts while this interface stays visible and locked.</p>
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
    { id: 1, enabled: true, depth: 0.75, sensitivity: 0.75, timing: 0.5, latch: 0.5, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
    { id: 2, enabled: true, depth: 0.75, sensitivity: 0.75, timing: 0.5, latch: 0.5, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
    { id: 3, enabled: true, depth: 0.75, sensitivity: 0.75, timing: 0.5, latch: 0.5, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
    { id: 4, enabled: true, depth: 0.75, sensitivity: 0.75, timing: 0.5, latch: 0.5, pairKey: '', inputKey: '', outputKey: '', inputChannel: -1, outputChannel: -1 },
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
  const recurringCutAlertTimer = useRef<number | null>(null);
  const [recurringCutAlert, setRecurringCutAlert] = useState<RecurringCutAlert | null>(null);
  const sensitivityCommandTimer = useRef<number | null>(null);
  const pendingSensitivityCommand = useRef<{ route: number; value: number } | null>(null);
  const bypassInitialized = useRef(false);
  const clearRecurringCutSession = () => {
    recurringCutHistory.current = {};
    previousRouteNotches.current = {};
    recurringCutAlertRef.current = null;
    if (recurringCutAlertTimer.current !== null) {
      window.clearTimeout(recurringCutAlertTimer.current);
      recurringCutAlertTimer.current = null;
    }
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
  const restartAvailable = nativeReady && !telemetry.calibrating;
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
    rawResponseDb: activeRouteTelemetry.calibrationRawResponseDb ?? activeRouteTelemetry.calibrationResponseDb,
  } : undefined);
  const activeRouteSelection = routeSelection(activeRouteState);
  const activeRouteMapped = !!activeRouteSelection.input
    && !!activeRouteSelection.output
    && activeRouteSelection.input.compatibilityKey === activeRouteSelection.output.compatibilityKey;
  const activeSpectrum = activeRouteTelemetry?.spectrumDb ?? telemetry.spectrumDb;
   const activeNotches = activeRouteTelemetry?.notches ?? telemetry.notches;
   const activeNotchCapacity = activeRouteTelemetry?.maximumAllowedNotches ?? 8;
   const activeNotchCount = activeNotches?.filter((notch) =>
     Number.isFinite(notch.frequency) && notch.frequency > 0
       && Number.isFinite(notch.depthDb) && notch.depthDb < -0.5).length
     ?? activeRouteTelemetry?.activeNotches
     ?? telemetry.activeNotches
     ?? 0;
   const activeDepth = activeRouteTelemetry?.depth ?? activeRouteTelemetry?.suppression ?? activeRouteState?.depth ?? 0.75;
   const activeSensitivity = activeRouteTelemetry?.sensitivity ?? activeRouteState?.sensitivity ?? 0.75;
   const activeTiming = activeRouteTelemetry?.timing ?? activeRouteState?.timing ?? 0.5;
   const activeLatch = activeRouteTelemetry?.latch ?? activeRouteState?.latch ?? 0.5;
   const timingFrameMs = 256 / (rate ?? 48000) * 1000;
   const timingHoldMs = Math.round((10 + 50 * activeTiming) * timingFrameMs);
   const observedCutDepthDb = activeRouteTelemetry?.maximumCutDb;
   const estimatedCutDepthDb = observedCutDepthDb !== undefined && observedCutDepthDb < -0.5
     ? Math.abs(observedCutDepthDb) : 32;
    const releaseStepDbPerFrame = Math.max(0.001,
      (0.15 - 0.08 * activeDepth)
     * (1 - 0.7 * activeTiming)
     * (1 - 0.45 * activeLatch));
   const timingReleaseMs = Math.round(
     estimatedCutDepthDb / releaseStepDbPerFrame * timingFrameMs);
   const timingReadout = `hold ${timingHoldMs} ms · release ${timingReleaseMs.toLocaleString('en-US')} ms`;
    const latchHoldBands = Math.min(activeNotchCapacity,
      Math.floor(activeNotchCapacity * (2 / 3) * Math.max(0, Math.min(1, activeLatch))));
    const latchReadout = `${latchHoldBands} ${latchHoldBands === 1 ? 'band' : 'bands'} max`;
    const maximumDepthDb = maximumDepthDbForAmount(activeDepth);
    const depthReadout = `−${maximumDepthDb.toFixed(1)} dB max`;
    const sensitivityThresholdDb = -70 * Math.max(0, Math.min(1, activeSensitivity));
    const sensitivityThresholdSign = sensitivityThresholdDb >= 0 ? '+' : '−';
    const sensitivityReadout = `${sensitivityThresholdSign}${Math.abs(sensitivityThresholdDb).toFixed(1)} dBFS base`;
   const expectedCallbackMs = rate && actualBuffer ? (actualBuffer / rate) * 1000 : undefined;
   const callbackJitterScore = audioRunning && expectedCallbackMs && typeof telemetry.callbackJitterMs === 'number'
     ? Math.max(0, Math.min(1, 1 - telemetry.callbackJitterMs / expectedCallbackMs))
     : undefined;
   const callbackJitterTone = callbackJitterScore === undefined ? undefined : callbackJitterScore >= 0.98 ? 'stable' : callbackJitterScore >= 0.9 ? 'watch' : 'unstable';
   const deviceClockReady = audioRunning && telemetry.deviceClockReady === true && typeof telemetry.deviceClockDriftPpm === 'number';
   const deviceClockPpm = deviceClockReady ? telemetry.deviceClockDriftPpm : undefined;
   const deviceClockTone = deviceClockPpm === undefined ? undefined : Math.abs(deviceClockPpm) <= 50 ? 'stable' : Math.abs(deviceClockPpm) <= 200 ? 'watch' : 'unstable';
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
        const rawResponseDb = Array.isArray(payload.rawResponseDb)
          ? payload.rawResponseDb.filter((value): value is number => typeof value === 'number' && Number.isFinite(value))
          : [];
        if (route > 0 && responseDb.length > 1) {
          setCalibrations((current) => ({ ...current, [route]: {
            delayMs: Number(payload.delayMs ?? 0),
            responseDb,
            rawResponseDb: rawResponseDb.length > 1 ? rawResponseDb : responseDb,
          } }));
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

   useEffect(() => () => {
     if (sensitivityCommandTimer.current !== null)
       window.clearTimeout(sensitivityCommandTimer.current);
   }, []);

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
        depth: route.depth,
        sensitivity: route.sensitivity,
         timing: route.timing,
         latch: route.latch,
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
       const currentNotches = (snapshot.notches ?? [])
         .filter((notch) => Number.isFinite(notch.frequency) && notch.frequency > 0);
       const currentFrequencies = currentNotches.map((notch) => notch.frequency);
      const previousFrequencies = previousRouteNotches.current[snapshot.route] ?? [];
       const freshEngagements = currentNotches.filter((notch) =>
         !previousFrequencies.some((previous) => sameCutFrequency(previous, notch.frequency)));
      const recentEvents = (recurringCutHistory.current[snapshot.route] ?? [])
        .filter((event) => now - event.timestamp <= recurringCutWindowMs);
       freshEngagements.forEach((notch) => recentEvents.push({
         frequency: notch.frequency,
         timestamp: now,
         qualified: false,
       }));
       const configuredMaximumDepthDb = maximumDepthDbForAmount(
         snapshot.depth ?? snapshot.suppression ?? 0.75);
       const isMaximumDepthCut = (notch: Notch) =>
         configuredMaximumDepthDb > 1
         && Math.abs(notch.depthDb) >= configuredMaximumDepthDb * 0.92;
       const qualifiedEvents = recentEvents.map((event) => {
         const matchingNotch = currentNotches.find((notch) =>
           sameCutFrequency(notch.frequency, event.frequency));
         return matchingNotch && isMaximumDepthCut(matchingNotch)
           ? { ...event, qualified: true }
           : event;
       });
       recurringCutHistory.current[snapshot.route] = qualifiedEvents;
      previousRouteNotches.current[snapshot.route] = currentFrequencies;

       if (recurringCutAlertRef.current) return;
       const qualifyingFrequencies: number[] = [];
       qualifiedEvents.forEach((event) => {
         if (qualifyingFrequencies.some((frequency) => sameCutFrequency(frequency, event.frequency)))
           return;
         const matchingEvents = qualifiedEvents.filter((candidate) =>
           candidate.qualified && sameCutFrequency(candidate.frequency, event.frequency));
          if (matchingEvents.length >= recurringCutThreshold)
           qualifyingFrequencies.push(event.frequency);
       });
       if (qualifyingFrequencies.length > 0) {
         const alert = {
           route: snapshot.route,
           frequencies: qualifyingFrequencies.slice(0, maximumSuggestedCuts),
         };
        recurringCutAlertRef.current = alert;
        setRecurringCutAlert(alert);
         if (recurringCutAlertTimer.current !== null)
           window.clearTimeout(recurringCutAlertTimer.current);
         recurringCutAlertTimer.current = window.setTimeout(() => {
           recurringCutAlertRef.current = null;
           recurringCutAlertTimer.current = null;
           setRecurringCutAlert(null);
         }, recurringCutDisplayMs);
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
    void bridge.restartApp().catch((error: unknown) => {
      setRestartingAudio(false);
      showToast(error instanceof Error ? error.message : 'Unable to restart Werfeed Herzback');
    });
  };
   const setDepth = (value: number) => {
    const nextValue = Math.max(0, Math.min(1, value));
     setRoutes((items) => items.map((route) => route.id === activeRoute + 1 ? { ...route, depth: nextValue } : route));
    if (!bridge || !nativeReady || !audioRunning) return;
     void bridge.command('set_protection', { route: activeRoute, depth: nextValue }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change cut depth'));
   };
   const flushSensitivityCommand = () => {
      if (sensitivityCommandTimer.current !== null) {
        window.clearTimeout(sensitivityCommandTimer.current);
        sensitivityCommandTimer.current = null;
      }
      const pending = pendingSensitivityCommand.current;
      pendingSensitivityCommand.current = null;
      if (!pending || !bridge || !nativeReady || !audioRunning) return;
      void bridge.command('set_protection', {
        route: pending.route,
        sensitivity: pending.value,
      }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change feedback sensitivity'));
   };
   const setSensitivity = (value: number) => {
     const nextValue = Math.max(0, Math.min(1, value));
     setRoutes((items) => items.map((route) => route.id === activeRoute + 1 ? { ...route, sensitivity: nextValue } : route));
     if (!bridge || !nativeReady || !audioRunning) return;
      pendingSensitivityCommand.current = { route: activeRoute, value: nextValue };
      if (sensitivityCommandTimer.current !== null)
        window.clearTimeout(sensitivityCommandTimer.current);
      sensitivityCommandTimer.current = window.setTimeout(flushSensitivityCommand, 75);
  };
  const setTiming = (value: number) => {
    const nextValue = Math.max(0, Math.min(1, value));
    setRoutes((items) => items.map((route) => route.id === activeRoute + 1 ? { ...route, timing: nextValue } : route));
    if (!bridge || !nativeReady || !audioRunning) return;
    void bridge.command('set_protection', { route: activeRoute, timing: nextValue }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change notch timing'));
  };
  const setLatch = (value: number) => {
    const nextValue = Math.max(0, Math.min(1, value));
    setRoutes((items) => items.map((route) => route.id === activeRoute + 1 ? { ...route, latch: nextValue } : route));
    if (!bridge || !nativeReady || !audioRunning) return;
    void bridge.command('set_protection', { route: activeRoute, latch: nextValue }).catch((error: unknown) => showToast(error instanceof Error ? error.message : 'Unable to change notch latch'));
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
    {restartingAudio && <div className="app-restart-lock" role="alert" aria-live="assertive" aria-busy="true"><RefreshCw size={20} /><strong>Restarting engine</strong><span>The interface is locked until the engine is ready.</span></div>}
    <header className="app-header">
       <div className="brand-lockup"><div className="brand-mark" aria-label="Werfeed Herzback logo"><svg className="brand-symbol" viewBox="0 0 32 32" role="img" aria-hidden="true"><path className="brand-triangle" d="M16 3.5 29 27.5H3Z" /><path className="brand-eye" d="M8.5 15.5s2.8-4 7.5-4 7.5 4 7.5 4-2.8 4-7.5 4-7.5-4-7.5-4Z" /><circle className="brand-pupil" cx="16" cy="15.5" r="2.15" /></svg></div><div><div className="eyebrow">Arian Rezaie's Adaptive Feedback Control</div><h1 className="brand-title">Werfeed Herzback <span className="brand-byline">· by Arian Rezaie</span> <small className="brand-version">v{packageJson.version}</small></h1></div></div>
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
              <section className="panel route-analyzer-panel"><div className="panel-heading"><div><div className="section-kicker"><Radio size={14} /> route {activeRoute + 1} analyzer</div><h3 className="section-title">Live spectrum and adaptive cuts</h3><p className="section-note">The detector uses a virtual flat reference until calibration adds measured room weighting. The pale curve is the exact native detection floor, including bass/treble shaping, the sensitivity slider, and calibrated hotspot bias.</p></div><span className="slot-count">{activeNotchCount} / {activeNotchCapacity} cuts</span></div><Spectrum values={activeSpectrum} notches={activeNotches} holdMs={timingHoldMs} releaseMs={timingReleaseMs} sensitivity={activeSensitivity} preset={preset} calibrationResponse={activeCalibration?.responseDb ?? []} />{recurringCutAlert?.route === activeRoute + 1 && <div className="recurring-cut-alert" role="status" aria-live="polite"><span>Consider manual cuts at <strong>{recurringCutAlert.frequencies.map((frequency) => `${frequency.toLocaleString('en-US', { maximumFractionDigits: 0 })} Hz`).join(', ')}</strong>.</span></div>}</section>
         <section className={`panel route-protection-panel ${protectionActive ? '' : 'is-bypassed'}`}><div className="panel-heading"><div><div className="section-kicker"><SlidersHorizontal size={14} /> route {activeRoute + 1} protection</div><h3 className="section-title">{protectionActive ? 'Suppression armed' : 'Suppression bypassed'}</h3><p className="section-note">Speech uses the shaped floor: +10 dB through 150 Hz, logarithmically to 0 dB by 500 Hz, flat through 1.5 kHz, then logarithmically to −18 dB by 8 kHz. Music keeps a flat frequency floor; calibration can still add room-specific hotspot bias.</p></div><Badge tone={protectionActive ? 'green' : 'red'}>{protectionActive ? 'armed' : 'bypassed'}</Badge></div><div className="mode-switch"><button type="button" className={preset === 'speech' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(protectionActive, 'speech')}><Mic2 size={13} /> Speech</button><button type="button" className={preset === 'music' ? 'active' : ''} disabled={!nativeReady} onClick={() => setProtection(protectionActive, 'music')}><Waves size={13} /> Music</button></div><div className="protection-slider-stack"><div className="route-fader"><div className="fader-copy"><div className="curve-name"><TimerReset size={14} /> Timing <span className="slider-readout">{timingReadout}</span></div></div><div className="fader-control"><input className="suppression-slider" type="range" min="0" max="100" step="1" value={Math.round(activeTiming * 100)} disabled={!nativeReady} onChange={(event) => setTiming(Number(event.target.value) / 100)} aria-label={`Route ${activeRoute + 1} notch timing`} style={{ background: `linear-gradient(90deg, #d19a63 0%, #d19a63 ${Math.round(activeTiming * 100)}%, #4a3025 ${Math.round(activeTiming * 100)}%, #4a3025 100%)` }} /></div></div><div className="route-fader"><div className="fader-copy"><div className="curve-name"><LockKeyhole size={14} /> Latch <span className="slider-readout">{latchReadout}</span></div></div><div className="fader-control"><input className="suppression-slider" type="range" min="0" max="100" step="1" value={Math.round(activeLatch * 100)} disabled={!nativeReady} onChange={(event) => setLatch(Number(event.target.value) / 100)} aria-label={`Route ${activeRoute + 1} notch latch`} style={{ background: `linear-gradient(90deg, #d19a63 0%, #d19a63 ${Math.round(activeLatch * 100)}%, #4a3025 ${Math.round(activeLatch * 100)}%, #4a3025 100%)` }} /></div></div><div className="route-fader"><div className="fader-copy"><div className="curve-name"><SlidersHorizontal size={14} /> Depth <span className="slider-readout">{depthReadout}</span></div></div><div className="fader-control"><input className="suppression-slider" type="range" min="0" max="100" step="1" value={Math.round(activeDepth * 100)} disabled={!nativeReady} onChange={(event) => setDepth(Number(event.target.value) / 100)} aria-label={`Route ${activeRoute + 1} cut depth`} style={{ background: `linear-gradient(90deg, #d19a63 0%, #d19a63 ${Math.round(activeDepth * 100)}%, #4a3025 ${Math.round(activeDepth * 100)}%, #4a3025 100%)` }} /></div></div><div className="route-fader"><div className="fader-copy"><div className="curve-name"><Radio size={14} /> Sensitivity <span className="slider-readout">{sensitivityReadout}</span></div></div><div className="fader-control"><input className="suppression-slider" type="range" min="0" max="100" step="1" value={Math.round(activeSensitivity * 100)} disabled={!nativeReady} onChange={(event) => setSensitivity(Number(event.target.value) / 100)} onPointerUp={flushSensitivityCommand} onKeyUp={flushSensitivityCommand} aria-label={`Route ${activeRoute + 1} feedback sensitivity`} style={{ background: `linear-gradient(90deg, #d19a63 0%, #d19a63 ${Math.round(activeSensitivity * 100)}%, #4a3025 ${Math.round(activeSensitivity * 100)}%, #4a3025 100%)` }} /></div></div></div></section>
       <section className="panel telemetry-panel">
        <div className="panel-heading"><div><div className="section-kicker"><Gauge size={14} /> live readings</div><h3 className="section-title">Engine and signal health</h3><p className="section-note">Telemetry stays visible below the control sections so live operation can be monitored without moving the routing controls.</p></div><Badge tone={audioRunning ? 'green' : 'quiet'}>{audioRunning ? 'audio running' : nativeReady ? 'engine connected' : 'waiting for engine'}</Badge></div>
        <div className="metrics">{[
          { label: 'Callback CPU', value: value(telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100), unit: '%', icon: <Activity size={14} />, level: telemetry.callbackCpu === undefined ? undefined : telemetry.callbackCpu * 100 },
          { label: 'Round-trip buffer', value: value(latency, 2), unit: 'ms est.', icon: <Gauge size={14} />, level: undefined },
          { label: 'Callback jitter', value: value(telemetry.callbackJitterMs, 2), unit: `ms · peak ${value(telemetry.callbackJitterPeakMs, 2)}`, icon: <Timer size={14} />, level: callbackJitterScore === undefined ? undefined : callbackJitterScore * 100, tone: callbackJitterTone },
          { label: 'Device clock', value: deviceClockReady ? value(deviceClockPpm, 0) : audioRunning ? 'warming' : '—', unit: 'ppm', icon: <Waves size={14} />, level: deviceClockPpm === undefined ? undefined : Math.max(0, 100 - Math.min(100, Math.abs(deviceClockPpm) / 2)), tone: deviceClockTone },
        ].map((metric) => <div className="metric" key={metric.label}><div className="metric-head"><span>{metric.label}</span>{metric.icon}</div><div className={`metric-value ${metric.tone ?? ''}`}>{metric.value} <small>{metric.unit}</small></div><Meter level={metric.level} /></div>)}</div>
        <div className="telemetry-footnote">Clock source: {telemetry.clockMeasurementSource ?? '—'}{telemetry.deviceClockAgeMs === undefined ? '' : ` · window ${value(telemetry.deviceClockAgeMs / 1000, 1)} s`}</div>
        <div className="health-grid"><div className="health"><label>Sample rate</label><strong>{rate === undefined ? '—' : `${rate} Hz`}</strong></div><div className="health"><label>Buffer</label><strong>{actualBuffer === undefined ? '—' : `${actualBuffer} samples`}</strong></div><div className="health"><label>Input peak</label><strong>{peakPercent(telemetry.inputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.inputPeak))}%`}</strong></div><div className="health"><label>Output peak</label><strong>{peakPercent(telemetry.outputPeak) === undefined ? '—' : `${value(peakPercent(telemetry.outputPeak))}%`}</strong></div></div>
       </section>
       </section>
    </main>
    <CalibrationDialog route={activeRoute + 1} record={showCalibration ? activeCalibration : undefined} live={activeSpectrum} onClose={() => setShowCalibration(false)} />
  </div>;
}

function App() { return <QueryClientProvider client={queryClient}><TooltipProvider><ErrorBoundary><Home /></ErrorBoundary><Toaster /></TooltipProvider></QueryClientProvider>; }
export default App;