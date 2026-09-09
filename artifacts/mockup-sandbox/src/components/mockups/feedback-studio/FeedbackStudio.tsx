import { useEffect, useMemo, useState, type ReactNode } from "react";
import {
  Activity,
  AlertTriangle,
  ArrowRight,
  Check,
  ChevronDown,
  CircleHelp,
  Cpu,
  Headphones,
  Info,
  LockKeyhole,
  Mic2,
  Music2,
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

type Stage = "configure" | "impulse" | "sweep" | "review" | "live";
type Pair = { id: number; input: string; output: string; name: string; delay: string };

const deviceOptions = [
  { name: "RME Fireface UCX II", meta: "USB · 18 × 20 · 48 kHz" },
  { name: "Focusrite Scarlett 18i20", meta: "USB · 18 × 20 · 48 kHz" },
  { name: "Dante Virtual Soundcard", meta: "Network · 64 × 64 · 48 kHz" },
];

const startingPairs: Pair[] = [
  { id: 1, input: "Input 1", output: "Output 1", name: "Wireless Mic Raw", delay: "2.8 ms" },
  { id: 2, input: "Input 2", output: "Output 2", name: "Wireless Mic Processed", delay: "2.9 ms" },
  { id: 3, input: "Input 3", output: "Output 3", name: "Lectern Mic Raw", delay: "3.1 ms" },
  { id: 4, input: "Input 4", output: "Output 4", name: "Lectern Mic Processed", delay: "3.0 ms" },
];

const cuts = [
  { hz: "248 Hz", db: "-3.2", q: "8.0", color: "#d99f62" },
  { hz: "1.26 kHz", db: "-4.7", q: "10.4", color: "#cb775e" },
  { hz: "2.51 kHz", db: "-2.1", q: "7.2", color: "#8e9e80" },
];

function Pill({ children, tone = "dim" }: { children: ReactNode; tone?: "dim" | "green" | "amber" | "red" }) {
  const colors = {
    dim: "border-[#34434a] bg-[#1b282d] text-[#a7b7b6]",
    green: "border-[#3d6556] bg-[#1c332e] text-[#9bd0af]",
    amber: "border-[#69533a] bg-[#392e21] text-[#e3b57e]",
    red: "border-[#70433e] bg-[#3e2827] text-[#e0a096]",
  };
  return <span className={`inline-flex items-center gap-1.5 rounded-full border px-2.5 py-1 text-[10px] font-semibold tracking-[.14em] uppercase ${colors[tone]}`}>{children}</span>;
}

function Knob({ label, value, accent = "#d99f62" }: { label: string; value: string; accent?: string }) {
  return (
    <div className="flex items-center gap-2.5">
      <div className="relative h-8 w-8 rounded-full border border-[#44545a] bg-[#192328] shadow-[inset_0_2px_5px_#0b1012]">
        <div className="absolute left-1/2 top-[2px] h-3 w-px -translate-x-1/2 origin-[50%_14px] rotate-[32deg]" style={{ background: accent }} />
        <div className="absolute inset-[5px] rounded-full border border-[#2d3b40]" />
      </div>
      <div><div className="text-[10px] uppercase tracking-[.14em] text-[#7f9395]">{label}</div><div className="font-mono text-[11px] text-[#d7e0d9]">{value}</div></div>
    </div>
  );
}

function Meter({ level = 62, hot = false }: { level?: number; hot?: boolean }) {
  return <div className="flex h-2.5 gap-[2px]">{Array.from({ length: 14 }, (_, i) => <span key={i} className={`w-[5px] rounded-[1px] ${i / 14 < level / 100 ? i > 10 ? hot ? "bg-[#c66a56]" : "bg-[#d99f62]" : "bg-[#8cae91]" : "bg-[#29363a]"}`} />)}</div>;
}

function Spectrum({ live = false }: { live?: boolean }) {
  const bars = useMemo(() => Array.from({ length: 54 }, (_, i) => {
    const wave = 22 + Math.sin(i * 0.56) * 13 + Math.sin(i * 1.9) * 7 + (i > 25 && i < 38 ? 13 : 0);
    return Math.max(7, Math.min(74, wave));
  }), []);
  return (
    <div className="relative h-[136px] overflow-hidden rounded-lg border border-[#25363b] bg-[#111a1e] px-3 pb-5 pt-3">
      <div className="pointer-events-none absolute inset-0 opacity-40" style={{ backgroundImage: "linear-gradient(#304349 1px,transparent 1px),linear-gradient(90deg,#304349 1px,transparent 1px)", backgroundSize: "100% 34px, 9% 100%" }} />
      <div className="relative flex h-full items-end gap-[3px]">
        {bars.map((height, i) => <span key={i} className="flex-1 rounded-t-sm bg-gradient-to-t from-[#607d76] to-[#b6bd86] opacity-80" style={{ height: `${live ? height + (i % 7 === 0 ? 17 : 0) : height}%` }} />)}
      </div>
      <div className="absolute bottom-1 left-3 right-3 flex justify-between font-mono text-[9px] text-[#718386]"><span>20 Hz</span><span>100 Hz</span><span>1 kHz</span><span>10 kHz</span><span>20 kHz</span></div>
    </div>
  );
}

function ChannelStrip({ pair, selected, onSelect, enabled }: { pair: Pair; selected: boolean; onSelect: () => void; enabled: boolean }) {
  return (
    <button onClick={onSelect} className={`group min-w-[170px] flex-1 rounded-xl border p-3 text-left transition ${selected ? "border-[#d99f62] bg-[#202d30] shadow-[0_0_0_1px_#5d4633]" : "border-[#293b40] bg-[#172226] hover:border-[#526666]"}`}>
      <div className="mb-3 flex items-center justify-between"><span className="font-mono text-[10px] text-[#789092]">{String(pair.id).padStart(2, "0")} / {pair.input.replace("Input ", "IN ")}</span><span className={`h-2 w-2 rounded-full ${enabled ? "bg-[#91bd9b]" : "bg-[#4a5b5d]"}`} /></div>
      <div className="mb-4 truncate text-sm font-semibold text-[#dbe3d9]">{pair.name}</div>
      <Meter level={52 + pair.id * 8} />
      <div className="mt-2 flex items-center justify-between text-[10px] text-[#7d9190]"><span>{pair.output}</span><span className="font-mono text-[#b5c2b6]">−∞ dB</span></div>
      <div className="mt-4 flex gap-2"><span className="rounded border border-[#34484a] px-1.5 py-1 text-[9px] uppercase tracking-widest text-[#7f9691]">HPF 80</span><span className="rounded border border-[#34484a] px-1.5 py-1 text-[9px] uppercase tracking-widest text-[#7f9691]">SAFE</span></div>
    </button>
  );
}

export function FeedbackStudio() {
  const [stage, setStage] = useState<Stage>("configure");
  const [device, setDevice] = useState(deviceOptions[0].name);
  const [pairs, setPairs] = useState(startingPairs);
  const [output, setOutput] = useState("Output 1");
  const [countdown, setCountdown] = useState<number | null>(null);
  const [progress, setProgress] = useState(0);
  const [selected, setSelected] = useState(1);
  const [mode, setMode] = useState<"speech" | "music">("speech");
  const [bypassed, setBypassed] = useState(false);
  const [armed, setArmed] = useState(true);

  useEffect(() => {
    if (countdown === null) return;
    if (countdown === 0) {
      setCountdown(null);
      if (stage === "impulse") {
        setStage("sweep");
        setCountdown(3);
        setProgress(45);
      } else {
        setProgress(100);
      }
      return;
    }
    const timer = window.setTimeout(() => setCountdown((n) => (n === null ? null : n - 1)), 900);
    return () => window.clearTimeout(timer);
  }, [countdown, stage]);

  const beginCalibration = () => { setStage("impulse"); setProgress(0); setCountdown(3); };
  const editName = (id: number, name: string) => setPairs((current) => current.map((pair) => pair.id === id ? { ...pair, name } : pair));
  const primaryLabel = stage === "configure" ? "Begin room calibration" : stage === "review" ? "Enter Live mode" : "Continue";

  return (
    <div className="min-h-screen overflow-x-auto bg-[#0d1518] font-['DM_Sans'] text-[#d4ded7]">
      <div className="min-w-[1060px]">
        <header className="flex h-[72px] items-center justify-between border-b border-[#26373b] bg-[#111c20] px-8">
           <div className="flex items-center gap-5"><div className="flex h-9 w-9 items-center justify-center rounded border border-[#ad7950] bg-[#30251e] text-[#d99f62]"><Waves size={20} /></div><div><div className="flex items-center gap-3"><h1 className="font-['Space_Mono'] text-[15px] font-bold tracking-[.08em] text-[#e4e7d9]">WERFEED HERZBACK</h1><Pill tone={stage === "live" ? "green" : "amber"}>{stage === "live" ? "LIVE" : "SETUP"}</Pill></div><p className="mt-1 text-[10px] tracking-[.22em] text-[#718587] uppercase">Precision feedback eliminator · rehearsal room 04</p></div></div>
          <div className="flex items-center gap-6"><div className="text-right"><div className="font-mono text-[10px] uppercase tracking-widest text-[#657a7d]">Engine</div><div className="flex items-center gap-2 text-[11px] text-[#a9c1b0]"><span className="h-1.5 w-1.5 rounded-full bg-[#8fbf9d]" /> Simulated native audio engine</div></div><button className="rounded-md border border-[#314448] p-2 text-[#94a6a4] hover:bg-[#1a292d]"><CircleHelp size={16} /></button></div>
        </header>

        <main className="grid grid-cols-[236px_1fr]">
          <aside className="min-h-[calc(100vh-72px)] border-r border-[#26373b] bg-[#101a1e] p-5">
            <div className="mb-8"><div className="mb-3 text-[10px] font-bold tracking-[.18em] text-[#718688] uppercase">Session map</div>{[["01", "I/O configuration", "configure"], ["02", "Impulse & delay", "impulse"], ["03", "Room sweep", "sweep"], ["04", "Calibration findings", "review"], ["05", "Live protection", "live"]].map(([num, label, key], i) => <div key={key} className={`relative flex gap-3 py-3 ${stage === key ? "text-[#d99f62]" : i < ["configure", "impulse", "sweep", "review", "live"].indexOf(stage) ? "text-[#9eb6a1]" : "text-[#617577]"}`}><div className={`z-10 flex h-5 w-5 items-center justify-center rounded-full border font-mono text-[9px] ${stage === key ? "border-[#d99f62] bg-[#3a2b21]" : "border-[#405356]"}`}>{i < ["configure", "impulse", "sweep", "review", "live"].indexOf(stage) ? <Check size={11} /> : num}</div><div className="pt-0.5 text-[11px] font-medium">{label}</div>{i < 4 && <div className="absolute left-[9px] top-8 h-5 border-l border-dashed border-[#304448]" />}</div>)}</div>
             <div className="border-t border-[#26373b] pt-5"><div className="mb-3 flex items-center gap-2 text-[10px] font-bold tracking-[.18em] text-[#718688] uppercase"><LockKeyhole size={12} /> Session note</div><div className="mb-3 flex items-end gap-3"><img src="/__mockup/images/feedback-studio-herzog-cutout.png" alt="Illustrated Werner Herzog portrait" className="h-16 w-14 object-contain object-bottom opacity-90" /><p className="font-['Playfair_Display'] text-[14px] italic leading-relaxed text-[#a2b1aa]">“The room tells you what it is. Calibration is the act of listening without argument.”</p></div><p className="mt-3 text-[10px] tracking-widest text-[#677b7d] uppercase">Field notebook · W. Herzog</p></div>
            <div className="mt-8 rounded-lg border border-[#2e4144] bg-[#172326] p-3"><div className="flex items-center justify-between text-[10px] text-[#77908e]"><span>CPU headroom</span><span className="font-mono text-[#b6c9b9]">68%</span></div><div className="mt-2 h-1.5 rounded-full bg-[#293a3d]"><div className="h-full w-[68%] rounded-full bg-[#88ad8e]" /></div><div className="mt-2 flex justify-between font-mono text-[9px] text-[#627779]"><span>12.4% used</span><span>48 kHz</span></div></div>
          </aside>

          <section className="p-8">
            <div className="mb-7 flex items-end justify-between"><div><div className="mb-2 flex items-center gap-2 text-[10px] font-bold tracking-[.18em] text-[#a37b56] uppercase"><Radio size={13} /> {stage === "live" ? "Protection surface" : "First-run calibration"}</div><h2 className="font-['Space_Mono'] text-2xl font-bold tracking-tight text-[#e0e6dc]">{stage === "configure" ? "Prepare the room." : stage === "impulse" ? "Finding the distance." : stage === "sweep" ? "Listening across the room." : stage === "review" ? "The room has spoken." : "Live protection."}</h2><p className="mt-2 max-w-2xl text-sm text-[#829695]">{stage === "configure" ? "Name the paths you recognize. Werfeed Herzback will keep the technical details out of your way once the system is running." : stage === "live" ? "Narrow-band protection is active. The musical signal remains untouched outside the cuts below." : "A short, confidence-building measurement. Keep the room quiet while the pass runs."}</p></div><div className="flex items-center gap-3"><Pill tone="green"><span className="h-1.5 w-1.5 rounded-full bg-[#9bd0af]" /> Signal path ready</Pill><button onClick={() => { setStage("configure"); setCountdown(null); setProgress(0); }} className="rounded-md border border-[#304348] p-2 text-[#8da3a2] hover:bg-[#1b2b2f]" title="Reset session"><RotateCcw size={15} /></button></div></div>

            {stage === "configure" && <div className="grid grid-cols-[1fr_280px] gap-5">
              <div className="rounded-xl border border-[#2d4145] bg-[#142125] p-5">
                <div className="mb-5 flex items-center justify-between"><div><h3 className="text-sm font-semibold text-[#dbe4d9]">Audio interface</h3><p className="mt-1 text-xs text-[#768b8d]">The device Werfeed Herzback will listen through.</p></div><Headphones size={20} className="text-[#8ea69b]" /></div>
                <div className="relative"><select value={device} onChange={(e) => setDevice(e.target.value)} className="w-full appearance-none rounded-lg border border-[#405458] bg-[#1d2c30] px-4 py-3 text-sm text-[#dfe6dd] outline-none focus:border-[#d99f62]">{deviceOptions.map((item) => <option key={item.name}>{item.name}</option>)}</select><ChevronDown className="pointer-events-none absolute right-3 top-3.5 text-[#8fa09d]" size={16} /></div><div className="mt-2 flex justify-between px-1 text-[10px] text-[#718688]"><span>{deviceOptions.find((d) => d.name === device)?.meta}</span><span className="text-[#91bd9b]">Connected</span></div>
                 <div className="my-6 border-t border-[#293b3f]" /><div className="mb-4 flex items-end justify-between"><div><h3 className="text-sm font-semibold text-[#dbe4d9]">Input / output pairs</h3><p className="mt-1 text-xs text-[#768b8d]">Each path is mono. Inputs and outputs stay numbered so you can route them in your interface.</p></div><span className="font-mono text-[10px] text-[#718688]">{pairs.length} / 8 PAIRS</span></div>
                 <div className="grid grid-cols-[30px_115px_115px_1fr] gap-3 px-3 pb-2 text-[9px] font-bold uppercase tracking-widest text-[#617779]"><span>#</span><span>Mono input</span><span>Mono output</span><span>Friendly name</span></div><div className="space-y-2">{pairs.map((pair) => <div key={pair.id} className="grid grid-cols-[30px_115px_115px_1fr] items-center gap-3 rounded-lg border border-[#293b3e] bg-[#18272b] px-3 py-2"><span className="font-mono text-[10px] text-[#71898b]">{String(pair.id).padStart(2, "0")}</span><span className="text-xs text-[#a7b8b0]">{pair.input}</span><span className="text-xs text-[#a7b8b0]">{pair.output}</span><input aria-label={`Friendly name for ${pair.input}`} value={pair.name} onChange={(e) => editName(pair.id, e.target.value)} className="rounded border border-transparent bg-[#213236] px-2.5 py-1.5 text-xs text-[#e0e6db] outline-none focus:border-[#b17d53]" /></div>)}</div>
                 {pairs.length < 8 && <button onClick={() => setPairs([...pairs, { id: pairs.length + 1, input: `Input ${pairs.length + 1}`, output: `Output ${pairs.length + 1}`, name: "Unassigned mono path", delay: "—" }])} className="mt-3 flex items-center gap-2 text-[10px] font-bold uppercase tracking-widest text-[#a8825d] hover:text-[#d8a46e]"><span className="text-lg leading-none">+</span> Add mono pair</button>}
              </div>
               <div className="rounded-xl border border-[#2d4145] bg-[#17262a] p-5"><div className="mb-5 flex items-center gap-3"><div className="rounded-md bg-[#302b21] p-2 text-[#d99f62]"><Volume2 size={17} /></div><div><h3 className="text-sm font-semibold text-[#dbe4d9]">Calibration output</h3><p className="mt-1 text-xs text-[#768b8d]">Choose one mono output to feed the room.</p></div></div><select value={output} onChange={(e) => setOutput(e.target.value)} className="w-full rounded-lg border border-[#405458] bg-[#1d2c30] px-3 py-2.5 text-xs text-[#dfe6dd] outline-none">{pairs.map((pair) => <option key={pair.output}>{pair.output}</option>)}</select><div className="mt-3 flex items-center gap-2 text-[10px] uppercase tracking-widest text-[#8eaa9b]"><Check size={12} /> Mono line-level signal · user-routed</div><div className="mt-8 rounded-lg border border-[#33494b] bg-[#1c2c2f] p-4"><div className="flex items-center gap-2 text-[10px] font-bold uppercase tracking-widest text-[#91aa9e]"><Info size={13} /> Before you begin</div><p className="mt-3 text-xs leading-relaxed text-[#889b98]">Mute open microphones. Route this output to the speaker or system you want to measure. Werfeed Herzback will not assume a stereo destination.</p></div><button onClick={beginCalibration} className="mt-6 flex w-full items-center justify-center gap-2 rounded-lg bg-[#b7794e] px-4 py-3 text-xs font-bold tracking-wide text-[#191818] transition hover:bg-[#d2935d]">{primaryLabel}<ArrowRight size={15} /></button></div>
            </div>}

            {(stage === "impulse" || stage === "sweep") && <div className="mx-auto max-w-3xl rounded-xl border border-[#304449] bg-[#142326] p-10 text-center"><div className="mx-auto mb-6 flex h-20 w-20 items-center justify-center rounded-full border border-[#a77850] bg-[#30251f] text-[#d99f62]">{stage === "impulse" ? <Activity size={32} /> : <Waves size={32} />}</div><Pill tone="amber">{stage === "impulse" ? "Impulse / delay pass" : "20 Hz — 20 kHz sweep"}</Pill><h3 className="mt-5 font-['Space_Mono'] text-3xl font-bold text-[#e4e8db]">{countdown !== null ? `Starting in ${countdown}` : stage === "impulse" ? "Measuring distance." : "Sweep complete."}</h3><p className="mx-auto mt-3 max-w-md text-sm leading-relaxed text-[#829694]">{stage === "impulse" ? "The room is quiet. We are finding when each path arrives." : "A one-second sweep has crossed the room. Resonances are being separated from the music."}</p><div className="mx-auto mt-9 max-w-lg"><div className="mb-2 flex justify-between text-[10px] uppercase tracking-widest text-[#6f8586]"><span>{stage === "impulse" ? "Pass 1 of 2" : "Pass 2 of 2"}</span><span>{stage === "impulse" ? "45%" : "100%"}</span></div><div className="h-2 rounded-full bg-[#293a3e]"><div className="h-full rounded-full bg-[#a98058] transition-all duration-500" style={{ width: `${stage === "impulse" ? (countdown === null ? 45 : 12) : 100}%` }} /></div></div>{stage === "sweep" && countdown === null && <button onClick={() => { setStage("review"); setProgress(100); }} className="mt-8 inline-flex items-center gap-2 rounded-lg bg-[#b7794e] px-6 py-3 text-xs font-bold text-[#191818]">Review findings <ArrowRight size={15} /></button>}</div>}

            {stage === "review" && <div className="grid grid-cols-[1.25fr_.75fr] gap-5"><div className="rounded-xl border border-[#2d4145] bg-[#142125] p-5"><div className="mb-5 flex items-center justify-between"><div><h3 className="text-sm font-semibold text-[#dbe4d9]">Calibration findings</h3><p className="mt-1 text-xs text-[#768b8d]">Measured arrival time and likely room resonances.</p></div><Pill tone="green"><Check size={12} /> Pass complete</Pill></div><div className="mb-5 overflow-hidden rounded-lg border border-[#293c40]"><table className="w-full text-left"><thead className="bg-[#1a2b2f] text-[9px] uppercase tracking-widest text-[#73898a]"><tr><th className="px-4 py-3">Path</th><th className="px-4 py-3">Arrival</th><th className="px-4 py-3">Confidence</th><th className="px-4 py-3">Status</th></tr></thead><tbody>{pairs.map((pair) => <tr key={pair.id} className="border-t border-[#293c40] text-xs"><td className="px-4 py-3 text-[#d2ddd3]">{pair.name}</td><td className="px-4 py-3 font-mono text-[#b3c2b5]">{pair.delay}</td><td className="px-4 py-3"><span className="text-[#9ec2a5]">High</span></td><td className="px-4 py-3"><Check size={14} className="text-[#9ec2a5]" /></td></tr>)}</tbody></table></div><Spectrum /></div><div className="rounded-xl border border-[#2d4145] bg-[#17262a] p-5"><div className="mb-4 flex items-center gap-2"><AlertTriangle size={16} className="text-[#d99f62]" /><h3 className="text-sm font-semibold text-[#dbe4d9]">Resonances to protect</h3></div><p className="mb-5 text-xs leading-relaxed text-[#809291]">Three narrow bands were found with enough energy to become feedback under gain.</p>{cuts.map((cut) => <div key={cut.hz} className="mb-2 flex items-center justify-between rounded-lg border border-[#344348] bg-[#1d2c30] px-3 py-2.5"><span className="font-mono text-xs text-[#d2ddd2]">{cut.hz}</span><span className="font-mono text-[11px]" style={{ color: cut.color }}>{cut.db} dB</span><span className="font-mono text-[10px] text-[#809392]">Q {cut.q}</span></div>)}<button onClick={() => setStage("live")} className="mt-7 flex w-full items-center justify-center gap-2 rounded-lg bg-[#b7794e] px-4 py-3 text-xs font-bold text-[#191818]">Enter Live mode <Zap size={15} /></button></div></div>}

            {stage === "live" && <div className="space-y-5"><div className="flex items-center justify-between rounded-xl border border-[#3a5548] bg-[#172d29] px-5 py-4"><div className="flex items-center gap-4"><div className="flex h-10 w-10 items-center justify-center rounded-full bg-[#29483c] text-[#9dccaa]"><Power size={19} /></div><div><div className="flex items-center gap-2 text-sm font-semibold text-[#d9e6d8]">Protection is listening</div><div className="mt-1 text-xs text-[#8fa89b]">3 narrow-band cuts · calibration-aware response · {device}</div></div></div><div className="flex items-center gap-4"><div className="text-right"><div className="font-mono text-lg text-[#b7d2b9]">3.4 ms</div><div className="text-[9px] uppercase tracking-widest text-[#75918a]">round-trip latency</div></div><button onClick={() => setArmed(!armed)} className={`relative h-6 w-11 rounded-full transition ${armed ? "bg-[#799b7c]" : "bg-[#3a4a4b]"}`}><span className={`absolute top-1 h-4 w-4 rounded-full bg-[#e2e6d7] transition-transform ${armed ? "translate-x-6" : "translate-x-1"}`} /></button></div></div><div className="grid grid-cols-[1fr_300px] gap-5"><div className="rounded-xl border border-[#2d4145] bg-[#142125] p-5"><div className="mb-5 flex items-center justify-between"><div><h3 className="text-sm font-semibold text-[#dbe4d9]">Input channels</h3><p className="mt-1 text-xs text-[#768b8d]">Select a path to inspect its protection curve.</p></div><div className="flex rounded-lg border border-[#35494b] bg-[#1b2b2f] p-1"><button onClick={() => setMode("speech")} className={`flex items-center gap-1.5 rounded px-3 py-1.5 text-[10px] font-bold uppercase tracking-widest ${mode === "speech" ? "bg-[#a8754e] text-[#1b1b19]" : "text-[#849795]"}`}><Mic2 size={12} /> Speech</button><button onClick={() => setMode("music")} className={`flex items-center gap-1.5 rounded px-3 py-1.5 text-[10px] font-bold uppercase tracking-widest ${mode === "music" ? "bg-[#a8754e] text-[#1b1b19]" : "text-[#849795]"}`}><Music2 size={12} /> Music</button></div></div><div className="flex gap-3 overflow-x-auto pb-2">{pairs.map((pair) => <ChannelStrip key={pair.id} pair={pair} selected={selected === pair.id} onSelect={() => setSelected(pair.id)} enabled={armed && !bypassed} />)}</div><div className="mt-5 border-t border-[#293b3f] pt-5"><div className="mb-3 flex items-center justify-between"><div className="flex items-center gap-2 text-xs font-semibold text-[#c9d8ce]"><SlidersHorizontal size={15} className="text-[#d99f62]" /> Protection curve · {pairs[selected - 1]?.name}</div><div className="flex items-center gap-3"><Knob label="Threshold" value="-6.0 dB" /><Knob label="Release" value="180 ms" accent="#8cae91" /></div></div><Spectrum live /></div></div><div className="space-y-5"><div className="rounded-xl border border-[#2d4145] bg-[#17262a] p-5"><div className="mb-4 flex items-center justify-between"><h3 className="text-sm font-semibold text-[#dbe4d9]">Active cuts</h3><span className="font-mono text-[10px] text-[#76908d]">3 / 12 SLOTS</span></div>{cuts.map((cut) => <div key={cut.hz} className="mb-2 flex items-center justify-between rounded-lg border border-[#344348] bg-[#1d2c30] px-3 py-2.5"><span className="h-2 w-2 rounded-full" style={{ backgroundColor: cut.color }} /><span className="font-mono text-xs text-[#d2ddd2]">{cut.hz}</span><span className="font-mono text-[11px] text-[#b6c8b7]">{cut.db} dB</span><button className="text-[#718688] hover:text-[#d7a16a]"><X size={13} /></button></div>)}<button className="mt-2 flex items-center gap-2 text-[10px] uppercase tracking-widest text-[#a8825d]"><Sparkles size={13} /> Auto-protect armed</button></div><div className="rounded-xl border border-[#2d4145] bg-[#17262a] p-5"><div className="mb-3 flex items-center gap-2 text-[10px] font-bold uppercase tracking-widest text-[#829897]"><Cpu size={14} /> System health</div><div className="mb-3 flex items-end justify-between"><span className="text-xs text-[#819493]">CPU headroom</span><span className="font-mono text-xl text-[#b4cfb8]">68%</span></div><Meter level={68} /><div className="mt-4 flex justify-between text-[10px] text-[#6f8586]"><span>48 kHz / 24-bit</span><span>Buffer 128</span></div></div></div></div><div className="flex items-center justify-between rounded-lg border border-[#4d4132] bg-[#25251f] px-4 py-3"><div className="flex items-center gap-2 text-[11px] text-[#b6aa8d]"><Info size={14} className="text-[#d0a269]" /> Preview simulates the native audio engine. It does not claim browser-level low-latency I/O.</div><button onClick={() => setBypassed(!bypassed)} className={`flex items-center gap-2 rounded-md border px-3 py-2 text-[10px] font-bold uppercase tracking-widest ${bypassed ? "border-[#a26758] bg-[#492e2b] text-[#e2a094]" : "border-[#59614f] bg-[#30392f] text-[#b6d0b7]"}`}><Power size={13} /> {bypassed ? "Bypassed" : "Bypass protection"}</button></div></div>}
          </section>
        </main>
      </div>
    </div>
  );
}