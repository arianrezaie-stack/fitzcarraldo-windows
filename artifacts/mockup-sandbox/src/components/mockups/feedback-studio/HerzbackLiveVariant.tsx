import { useMemo, useState, type ReactNode } from "react";
import {
  Activity,
  AlertTriangle,
  AudioLines,
  Check,
  ChevronDown,
  CircleHelp,
  Gauge,
  LockKeyhole,
  Mic2,
  Power,
  Radio,
  RotateCcw,
  SlidersHorizontal,
  Sparkles,
  Volume2,
  Waves,
  X,
  Zap,
} from "lucide-react";

type Route = {
  id: number;
  input: string;
  output: string;
  name: string;
  delay: string;
  state: "locked" | "ready";
};

const routeSeed: Route[] = [
  { id: 1, input: "Input 1", output: "Output 1", name: "Wireless Mic Raw", delay: "2.8 ms", state: "locked" },
  { id: 2, input: "Input 2", output: "Output 2", name: "Wireless Mic Processed", delay: "2.9 ms", state: "ready" },
  { id: 3, input: "Input 3", output: "Output 3", name: "Lectern Mic Raw", delay: "3.1 ms", state: "locked" },
  { id: 4, input: "Input 4", output: "Output 4", name: "Lectern Mic Processed", delay: "3.0 ms", state: "ready" },
];

const activeCuts = [
  { hz: "248 Hz", db: "−3.2 dB", release: "holding", color: "#c99462", x: 31, y: 55 },
  { hz: "1.26 kHz", db: "−4.7 dB", release: "rising", color: "#b96348", x: 53, y: 38 },
  { hz: "2.51 kHz", db: "−2.1 dB", release: "holding", color: "#d1ad78", x: 63, y: 46 },
];

function Badge({
  children,
  tone = "quiet",
}: {
  children: ReactNode;
  tone?: "quiet" | "green" | "amber" | "red";
}) {
  const style = {
    quiet: "border-[#665044] bg-[#2b201b] text-[#c0aa92]",
    green: "border-[#80603f] bg-[#463322] text-[#e1c394]",
    amber: "border-[#9a6946] bg-[#513123] text-[#e1ae76]",
    red: "border-[#8d4c3c] bg-[#482823] text-[#e5a08d]",
  }[tone];

  return (
    <span className={`inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-[10px] font-bold uppercase tracking-[.15em] ${style}`}>
      {children}
    </span>
  );
}

function Meter({ level = 68, hot = false }: { level?: number; hot?: boolean }) {
  return (
    <div className="flex h-3 items-end gap-[3px]">
      {Array.from({ length: 18 }, (_, i) => {
        const active = i / 18 < level / 100;
        return (
          <span
            key={i}
            className={`w-[5px] rounded-[1px] transition-opacity ${
              active
                ? i > 14
                  ? hot
                    ? "bg-[#ba5d43]"
                    : "bg-[#bf8b59]"
                  : "bg-[#bd9970]"
                : "bg-[#4a3329]"
            }`}
            style={{ height: `${7 + (i % 4) * 2}px` }}
          />
        );
      })}
    </div>
  );
}

function Dial({ label, value, accent = "#c99462" }: { label: string; value: string; accent?: string }) {
  return (
    <div className="flex items-center gap-2.5">
      <div className="relative h-9 w-9 rounded-full border border-[#765a48] bg-[#241913] shadow-[inset_0_2px_6px_#120c09]">
        <span
          className="absolute left-1/2 top-[2px] h-3 w-px origin-[50%_15px] -translate-x-1/2 rotate-[34deg]"
          style={{ backgroundColor: accent }}
        />
        <span className="absolute inset-[6px] rounded-full border border-[#553d30]" />
      </div>
      <div>
        <div className="text-[9px] uppercase tracking-[.16em] text-[#a58a72]">{label}</div>
        <div className="font-mono text-[11px] text-[#eadbc5]">{value}</div>
      </div>
    </div>
  );
}

function Spectrum({
  live,
  averaging,
  timeConstant,
  cuts,
}: {
  live: boolean;
  averaging: string;
  timeConstant: string;
  cuts: typeof activeCuts;
}) {
  const points = useMemo(() => {
    const source = Array.from(
      { length: 72 },
      (_, i) =>
        Math.max(
          10,
          Math.min(
            88,
            33 +
              Math.sin(i * 0.23) * 9 +
              Math.sin(i * 0.71) * 5 +
              (i > 28 && i < 42 ? 17 : 0) +
              (i > 54 ? 8 : 0),
          ),
        ),
    );
    const windowSize = averaging === "1/1 octave" ? 7 : averaging === "1/6 octave" ? 2 : 4;
    return source.map((_, index) => {
      const slice = source.slice(Math.max(0, index - windowSize), Math.min(source.length, index + windowSize + 1));
      return slice.reduce((sum, value) => sum + value, 0) / slice.length;
    });
  }, [averaging]);

  const linePoints = points
    .map((height, index) => `${(index / (points.length - 1)) * 100},${100 - height - (live && index % 13 === 0 ? 5 : 0)}`)
    .join(" ");
  const areaPoints = `0,100 ${linePoints} 100,100`;

  return (
    <div className="relative h-[164px] overflow-hidden rounded-xl border border-[#65483a] bg-[#211611] px-4 pb-6 pt-4 shadow-[inset_0_0_0_1px_rgba(223,188,142,.04)]">
      <div
        className="pointer-events-none absolute inset-0 opacity-50"
        style={{
          backgroundImage:
            "linear-gradient(rgba(126,85,59,.22) 1px,transparent 1px),linear-gradient(90deg,rgba(126,85,59,.16) 1px,transparent 1px)",
          backgroundSize: "100% 35px, 9% 100%",
        }}
      />
      <div className="absolute left-3 top-3 text-[9px] uppercase tracking-[.18em] text-[#a88b70]">
        Protection envelope · live
      </div>
      <svg
        className="absolute inset-x-4 bottom-6 top-8 h-[112px] w-[calc(100%-2rem)] overflow-visible"
        viewBox="0 0 100 100"
        preserveAspectRatio="none"
        aria-label={`Smoothed analyzer line, ${averaging} averaging, ${timeConstant} time constant`}
      >
        <polygon points={areaPoints} fill="#bf8b59" fillOpacity=".08" />
        <polyline
          points={linePoints}
          fill="none"
          stroke="#e1bd83"
          strokeWidth="1.5"
          vectorEffect="non-scaling-stroke"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
        <polyline
          points={linePoints}
          fill="none"
          stroke="#a9644a"
          strokeOpacity=".28"
          strokeWidth="5"
          vectorEffect="non-scaling-stroke"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
        {live &&
          cuts.map((cut) => (
            <g key={cut.hz}>
              <line
                x1={cut.x}
                x2={cut.x}
                y1="12"
                y2="90"
                stroke={cut.color}
                strokeOpacity=".78"
                strokeDasharray="2 2"
                vectorEffect="non-scaling-stroke"
              />
              <circle cx={cut.x} cy={cut.y} r="2.4" fill={cut.color} stroke="#211611" strokeWidth="1" vectorEffect="non-scaling-stroke" />
              <text x={cut.x} y="10" textAnchor="middle" fill={cut.color} fontSize="3.1" fontFamily="ui-monospace, SFMono-Regular, Menlo, monospace">
                {cut.hz}
              </text>
            </g>
          ))}
      </svg>
      <div className="absolute bottom-9 left-4 flex flex-wrap items-center gap-3 text-[9px] text-[#c3a276]">
        <span className="flex items-center gap-2">
          <span className="h-px w-5 bg-[#e1bd83]" /> smoothed response
        </span>
        <span className="flex items-center gap-2 text-[#d29261]">
          <span className="h-3 w-px border-l border-dashed border-[#d29261]" /> active EQ move
        </span>
      </div>
      <div className="absolute bottom-2 left-4 right-4 flex justify-between font-mono text-[9px] text-[#9b7c63]">
        <span>20 Hz</span>
        <span>500 Hz</span>
        <span>2 kHz</span>
        <span>8 kHz</span>
        <span>20 kHz</span>
      </div>
    </div>
  );
}

export function HerzbackLiveVariant() {
  const [routes, setRoutes] = useState(routeSeed);
  const [selected, setSelected] = useState(2);
  const [armed, setArmed] = useState(true);
  const [bypassed, setBypassed] = useState(false);
  const [mode, setMode] = useState<"speech" | "music">("speech");
  const [averaging, setAveraging] = useState("1/3 octave");
  const [timeConstant, setTimeConstant] = useState("1 s");
  const [showToast, setShowToast] = useState(false);

  const toggleRoute = (id: number) =>
    setRoutes((items) =>
      items.map((route) =>
        route.id === id ? { ...route, state: route.state === "locked" ? "ready" : "locked" } : route,
      ),
    );

  const reset = () => {
    setRoutes(routeSeed);
    setSelected(2);
    setArmed(true);
    setBypassed(false);
    setShowToast(true);
    window.setTimeout(() => setShowToast(false), 1700);
  };

  return (
    <div
      className="min-h-[100dvh] bg-[#251914] px-4 py-5 text-[#eadbc5] md:px-8 lg:px-10"
      style={{
        fontFamily: "ui-sans-serif, system-ui, sans-serif",
        backgroundImage:
          "radial-gradient(circle at 12% 8%, rgba(190,137,88,.13), transparent 27%), repeating-linear-gradient(112deg, rgba(116,75,48,.12) 0 1px, transparent 1px 6px), linear-gradient(145deg, #2d1c16, #1f1511 72%)",
      }}
    >
      {showToast && (
        <div className="fixed right-5 top-5 z-20 flex items-center gap-2 rounded-lg border border-[#8a6044] bg-[#4a2d22] px-4 py-3 text-xs text-[#f0c795] shadow-xl">
          <Check size={15} /> Calibration snapshot restored
        </div>
      )}

      <header className="mx-auto flex max-w-[1440px] items-center justify-between border-b border-[#624635] pb-5">
        <div className="flex items-center gap-4">
          <div className="flex h-10 w-10 items-center justify-center rounded-xl border border-[#8b5f42] bg-[#3a241b] text-[#d59c61] shadow-[inset_0_0_0_1px_rgba(240,198,146,.08)]">
            <AudioLines size={20} />
          </div>
          <div>
            <div className="text-[10px] font-bold uppercase tracking-[.25em] text-[#ae8e73]">Werfeed / analog live guard</div>
            <h1 className="mt-1 text-lg font-semibold tracking-tight text-[#f0e0c8]" style={{ fontFamily: "Georgia, serif" }}>
              Werfeed Herzback <span className="font-normal text-[#b79679]">· calibrated protection</span>
            </h1>
          </div>
        </div>
        <div className="hidden items-center gap-3 md:flex">
          <Badge tone="green">
            <span className="h-1.5 w-1.5 rounded-full bg-[#d7a568]" /> Engine stable
          </Badge>
          <span className="font-mono text-[10px] text-[#a2876f]">SESSION 04:18:26</span>
          <button
            onClick={reset}
            className="rounded-lg border border-[#6a4a37] p-2 text-[#b99472] hover:border-[#c18a5a] hover:text-[#f0c18c]"
            title="Restore calibration"
          >
            <RotateCcw size={15} />
          </button>
        </div>
      </header>

      <main className="mx-auto max-w-[1440px] py-6">
        <section
          className={`relative overflow-hidden rounded-2xl border ${armed && !bypassed ? "border-[#9b704a]" : "border-[#65473a]"} bg-[#3a241b] p-5 shadow-[0_18px_50px_rgba(12,7,4,.22)] md:p-7`}
        >
          <div
            className="pointer-events-none absolute inset-0 opacity-60"
            style={{
              backgroundImage:
                "repeating-linear-gradient(0deg, rgba(232,191,138,.035) 0 1px, transparent 1px 4px), radial-gradient(circle at 82% 20%, rgba(195,139,83,.18), transparent 34%), linear-gradient(115deg, transparent 0 62%, rgba(133,74,49,.09) 62% 63%, transparent 63%)",
            }}
          />
          <div className="relative grid gap-7 lg:grid-cols-[1fr_320px]">
            <div>
              <div className="flex flex-wrap items-center gap-3">
                <Badge tone={armed && !bypassed ? "green" : "red"}>
                  {armed && !bypassed ? (
                    <>
                      <Radio size={12} /> live protection
                    </>
                  ) : (
                    <>
                      <Power size={12} /> protection off
                    </>
                  )}
                </Badge>
                <span className="font-mono text-[10px] uppercase tracking-[.18em] text-[#b19072]">Herzback bus / 01</span>
              </div>
              <div className="mt-5 flex flex-col justify-between gap-5 md:flex-row md:items-end">
                <div>
                  <h2 className="max-w-[620px] text-3xl font-semibold tracking-[-.04em] text-[#f1e3cc] md:text-5xl" style={{ fontFamily: "Georgia, serif" }}>
                    Catch feedback
                    <br />
                    <span className="text-[#d39a63]">before the room does.</span>
                  </h2>
                  <p className="mt-4 max-w-[520px] text-sm leading-6 text-[#c2a78b]">
                    A calibrated, low-latency guard for the live mix. Herzback listens to each mono path and makes only the cut the room needs.
                  </p>
                </div>
                <div className="flex items-center gap-3">
                  <button
                    onClick={() => setArmed(!armed)}
                    className={`flex items-center gap-2 rounded-lg border px-4 py-3 text-[11px] font-bold uppercase tracking-[.16em] transition-colors ${
                      armed ? "border-[#b07d4e] bg-[#71462f] text-[#f3c18a]" : "border-[#984c3c] bg-[#4f2924] text-[#e6a18e]"
                    }`}
                  >
                    <Zap size={15} /> {armed ? "Protection armed" : "Arm protection"}
                  </button>
                  <button
                    onClick={() => setBypassed(!bypassed)}
                    className={`rounded-lg border p-3 ${bypassed ? "border-[#a45140] text-[#e4a092]" : "border-[#75533e] text-[#c0a184]"}`}
                    title="Bypass protection"
                  >
                    <Power size={16} />
                  </button>
                </div>
              </div>
              <div className="mt-7 grid gap-3 md:grid-cols-3">
                {[
                  { label: "Suppression", value: "−3.8", unit: "dB", icon: <Activity size={14} className="text-[#d19a63]" />, level: 54, hot: true },
                  { label: "Response", value: "2.9", unit: "ms", icon: <Gauge size={14} className="text-[#d1ad78]" />, level: 32 },
                  { label: "Headroom", value: "68", unit: "%", icon: <Volume2 size={14} className="text-[#c69362]" />, level: 68 },
                ].map((metric) => (
                  <div key={metric.label} className="rounded-xl border border-[#714d39] bg-[#2b1b15]/85 p-4">
                    <div className="flex items-center justify-between text-[10px] uppercase tracking-[.16em] text-[#b18e72]">
                      <span>{metric.label}</span>
                      {metric.icon}
                    </div>
                    <div className="mt-2 font-mono text-3xl text-[#f0dfc6]">
                      {metric.value} <small className="text-sm text-[#b39578]">{metric.unit}</small>
                    </div>
                    <div className="mt-3">
                      <Meter level={metric.level} hot={metric.hot} />
                    </div>
                  </div>
                ))}
              </div>
            </div>
            <aside className="rounded-xl border border-[#876043] bg-[#4a2d21] p-5 shadow-[0_12px_32px_rgba(9,5,3,.24)]">
              <div className="flex items-center justify-between">
                <div className="flex items-center gap-2 text-[10px] font-bold uppercase tracking-[.17em] text-[#e0bc8e]">
                  <Sparkles size={14} className="text-[#e0a968]" /> live guard
                </div>
                <Badge tone="amber">48 kHz</Badge>
              </div>
              <div className="mt-7 flex items-center justify-center">
                <div className="relative flex h-40 w-40 items-center justify-center rounded-full border border-[#b17a4e] bg-[#6a412b] shadow-[0_0_0_12px_#513020,0_0_0_13px_#865b3e]">
                  <div className="text-center">
                    <div className="font-mono text-5xl tracking-[-.08em] text-[#f2d7ab]">ON</div>
                    <div className="mt-1 text-[9px] uppercase tracking-[.2em] text-[#d4ab79]">guarding</div>
                  </div>
                  <span className="absolute right-2 top-5 h-2 w-2 rounded-full bg-[#e1aa68] shadow-[0_0_0_4px_#68412b]" />
                </div>
              </div>
              <div className="mt-8 border-t border-[#80563e] pt-4 text-center">
                <div className="font-mono text-[11px] text-[#e0be91]">3 active cuts · 0 warnings</div>
                <div className="mt-2 text-[10px] leading-5 text-[#c09e7e]">
                  The live path is protected
                  <br />
                  without touching your tonal balance.
                </div>
              </div>
            </aside>
          </div>
        </section>

        <section className="mt-6 grid gap-6 xl:grid-cols-[1fr_340px]">
          <div className="rounded-2xl border border-[#664638] bg-[#302019] p-5 shadow-[0_14px_40px_rgba(12,7,4,.15)] md:p-6">
            <div className="mb-5 flex flex-wrap items-center justify-between gap-3">
              <div>
                <div className="flex items-center gap-2 text-[10px] font-bold uppercase tracking-[.18em] text-[#b28e72]">
                  <SlidersHorizontal size={14} className="text-[#d19a63]" /> calibrated mono routing
                </div>
                <h3 className="mt-2 text-lg font-semibold text-[#f0ddc1]" style={{ fontFamily: "Georgia, serif" }}>
                  Input channels
                </h3>
                <p className="mt-1 text-xs text-[#b29579]">Each path keeps its own learned protection curve.</p>
              </div>
              <div className="flex rounded-lg border border-[#684838] bg-[#241712] p-1">
                <button
                  onClick={() => setMode("speech")}
                  className={`flex items-center gap-2 rounded-md px-3 py-2 text-[10px] font-bold uppercase tracking-widest ${mode === "speech" ? "bg-[#b9774a] text-[#2b1b15]" : "text-[#b29476]"}`}
                >
                  <Mic2 size={13} /> Speech
                </button>
                <button
                  onClick={() => setMode("music")}
                  className={`flex items-center gap-2 rounded-md px-3 py-2 text-[10px] font-bold uppercase tracking-widest ${mode === "music" ? "bg-[#b9774a] text-[#2b1b15]" : "text-[#b29476]"}`}
                >
                  <Waves size={13} /> Music
                </button>
              </div>
            </div>

            <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
              {routes.map((route) => (
                <button
                  key={route.id}
                  onClick={() => setSelected(route.id)}
                  className={`group rounded-xl border p-4 text-left transition-colors ${
                    selected === route.id ? "border-[#b37e50] bg-[#483022]" : "border-[#624335] bg-[#261813] hover:border-[#966446]"
                  }`}
                >
                  <div className="flex items-center justify-between">
                    <span className={`h-2 w-2 rounded-full ${route.state === "locked" ? "bg-[#d49b61]" : "bg-[#b97b4d]"}`} />
                    <span className="font-mono text-[10px] text-[#a88a70]">CH 0{route.id}</span>
                  </div>
                  <div className="mt-5 truncate text-sm font-semibold text-[#ecdcc4]">{route.name}</div>
                  <div className="mt-2 flex items-center justify-between font-mono text-[10px] text-[#ac8e74]">
                    <span>
                      {route.input} → {route.output}
                    </span>
                    <span>{route.delay}</span>
                  </div>
                  <div className="mt-4 flex items-center justify-between border-t border-[#5d3e31] pt-3 text-[9px] font-bold uppercase tracking-[.13em] text-[#b2957a]">
                    <span>{route.state === "locked" ? "calibrated" : "monitoring"}</span>
                    <span
                      onClick={(event) => {
                        event.stopPropagation();
                        toggleRoute(route.id);
                      }}
                      className="cursor-pointer text-[#d29761]"
                    >
                      {route.state === "locked" ? "unlock" : "lock"}
                    </span>
                  </div>
                </button>
              ))}
            </div>

            <div className="mt-6 border-t border-[#5d3e31] pt-5">
              <div className="mb-3 flex flex-wrap items-center justify-between gap-3">
                <div className="flex items-center gap-2 text-xs font-semibold text-[#e5d5bc]">
                  <Radio size={15} className="text-[#d09a63]" /> Protection curve · {routes[selected - 1]?.name}
                </div>
                <div className="flex flex-wrap items-center gap-3">
                  <label className="flex items-center gap-2 text-[9px] font-bold uppercase tracking-[.13em] text-[#b08e72]">
                    Averaging
                    <select
                      value={averaging}
                      onChange={(event) => setAveraging(event.target.value)}
                      className="rounded-md border border-[#684839] bg-[#251712] px-2 py-1.5 font-mono text-[10px] normal-case tracking-normal text-[#dfcdb3] outline-none"
                    >
                      <option>1/1 octave</option>
                      <option>1/3 octave</option>
                      <option>1/6 octave</option>
                    </select>
                  </label>
                  <label className="flex items-center gap-2 text-[9px] font-bold uppercase tracking-[.13em] text-[#b08e72]">
                    Time constant
                    <select
                      value={timeConstant}
                      onChange={(event) => setTimeConstant(event.target.value)}
                      className="rounded-md border border-[#684839] bg-[#251712] px-2 py-1.5 font-mono text-[10px] normal-case tracking-normal text-[#dfcdb3] outline-none"
                    >
                      <option>125 ms</option>
                      <option>1 s</option>
                      <option>2 s</option>
                    </select>
                  </label>
                  <Dial label="Threshold" value="-6.0 dB" />
                  <Dial label="Release" value="180 ms" accent="#b77c4e" />
                </div>
              </div>
              <Spectrum live={armed && !bypassed} averaging={averaging} timeConstant={timeConstant} cuts={activeCuts} />
            </div>
          </div>

          <div className="space-y-6">
            <div className="rounded-2xl border border-[#664638] bg-[#302019] p-5 shadow-[0_14px_40px_rgba(12,7,4,.15)]">
              <div className="flex items-center justify-between">
                <h3 className="text-sm font-semibold text-[#f0ddc1]" style={{ fontFamily: "Georgia, serif" }}>
                  Live EQ moves
                </h3>
                <span className="font-mono text-[10px] text-[#ad8d72]">03 / 12 SLOTS</span>
              </div>
              <p className="mt-1 text-[10px] leading-4 text-[#b39478]">Narrow cuts currently held against the calibrated curve.</p>
              {activeCuts.map((cut) => (
                <div key={cut.hz} className="mt-3 rounded-lg border border-[#664638] bg-[#261813] px-3 py-2.5">
                  <div className="flex items-center justify-between">
                    <span className="flex items-center gap-2 font-mono text-xs text-[#eadbc5]">
                      <span className="h-2 w-2 rounded-full" style={{ backgroundColor: cut.color }} />
                      {cut.hz}
                    </span>
                    <span className="font-mono text-[11px] text-[#d1b38c]">{cut.db}</span>
                    <button onClick={() => setShowToast(true)} className="text-[#a8886d] hover:text-[#e0a36c]" title={`Release ${cut.hz} cut`}>
                      <X size={13} />
                    </button>
                  </div>
                  <div className="mt-2 flex items-center justify-between text-[9px] font-bold uppercase tracking-[.14em] text-[#a98a70]">
                    <span>release state</span>
                    <span className={cut.release === "rising" ? "text-[#d3975f]" : "text-[#c4a278]"}>{cut.release}</span>
                  </div>
                </div>
              ))}
              <div className="mt-4 flex items-center gap-2 text-[10px] font-bold uppercase tracking-widest text-[#d29a61]">
                <Sparkles size={13} /> Auto-protect armed
              </div>
            </div>

            <div className="rounded-2xl border border-[#664638] bg-[#302019] p-5 shadow-[0_14px_40px_rgba(12,7,4,.15)]">
              <div className="flex items-center gap-2 text-[10px] font-bold uppercase tracking-widest text-[#b29074]">
                <LockKeyhole size={14} /> Device & clock
              </div>
              <button className="mt-4 flex w-full items-center justify-between rounded-lg border border-[#684839] bg-[#261813] px-3 py-3 text-left">
                <span>
                  <span className="block text-xs font-semibold text-[#eadbc5]">RME Fireface UCX II</span>
                  <span className="mt-1 block font-mono text-[10px] text-[#a9886d]">USB · 18 × 20 · 48 kHz</span>
                </span>
                <ChevronDown size={15} className="text-[#b18e72]" />
              </button>
              <div className="mt-4 flex items-center justify-between text-xs text-[#b18f73]">
                <span>Buffer</span>
                <span className="font-mono text-[#e1c9a9]">128 samples</span>
              </div>
              <div className="mt-2 flex items-center justify-between text-xs text-[#b18f73]">
                <span>Clock source</span>
                <span className="flex items-center gap-1.5 font-mono text-[#d3a16a]">
                  <Check size={12} /> Internal
                </span>
              </div>
            </div>
          </div>
        </section>

        <footer className="mt-6 flex flex-wrap items-center justify-between gap-3 rounded-xl border border-[#76543d] bg-[#38261b] px-4 py-3 text-[11px] text-[#c0a487]">
          <div className="flex items-center gap-2">
            <AlertTriangle size={14} className="text-[#d09b61]" /> Preview simulates the native audio engine; browser I/O is not low-latency.
          </div>
          <div className="flex items-center gap-4">
            <button onClick={() => setShowToast(true)} className="flex items-center gap-2 text-[#d0a170] hover:text-[#f0c18d]">
              <CircleHelp size={14} /> Read calibration notes
            </button>
            <button
              onClick={() => setBypassed(!bypassed)}
              className={`flex items-center gap-2 rounded-md border px-3 py-2 font-bold uppercase tracking-widest ${
                bypassed ? "border-[#a26758] bg-[#492e2b] text-[#e2a094]" : "border-[#806248] bg-[#513521] text-[#e2c18f]"
              }`}
            >
              <Power size={13} /> {bypassed ? "Bypassed" : "Bypass protection"}
            </button>
          </div>
        </footer>
      </main>
    </div>
  );
}