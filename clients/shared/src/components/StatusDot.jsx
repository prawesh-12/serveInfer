const TONES = {
  online: { dot: 'bg-emerald-500 ring-emerald-500/25', text: 'text-emerald-700' },
  done: { dot: 'bg-emerald-500 ring-emerald-500/25', text: 'text-emerald-700' },
  streaming: { dot: 'bg-blue-500 ring-blue-500/25 animate-pulse', text: 'text-blue-700' },
  queued: { dot: 'bg-amber-500 ring-amber-500/25 animate-pulse', text: 'text-amber-700' },
  checking: { dot: 'bg-amber-500 ring-amber-500/25 animate-pulse', text: 'text-amber-700' },
  offline: { dot: 'bg-red-500 ring-red-500/25', text: 'text-red-700' },
  error: { dot: 'bg-red-500 ring-red-500/25', text: 'text-red-700' },
  idle: { dot: 'bg-neutral-400 ring-neutral-400/25', text: 'text-neutral-600' },
  cancelled: { dot: 'bg-neutral-500 ring-neutral-500/25', text: 'text-neutral-600' },
};

const FALLBACK = TONES.idle;

export function StatusDot({ state, label }) {
  const tone = TONES[state] || FALLBACK;
  const text = label ?? state ?? 'unknown';

  return (
    <span className="inline-flex min-w-0 items-center gap-1.5" title={`${text} (${state})`}>
      <span className={`size-2.5 shrink-0 rounded-full ring-3 ${tone.dot}`} />
      <span className={`truncate text-[11px] leading-none font-semibold tracking-wide ${tone.text}`}>{text}</span>
    </span>
  );
}

export default StatusDot;
