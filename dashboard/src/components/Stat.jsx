const DOT = {
  ok: 'bg-emerald-500 ring-emerald-500/25',
  warn: 'bg-amber-500 ring-amber-500/25',
  bad: 'bg-red-500 ring-red-500/25',
};

export default function Stat({ label, value, dot }) {
  return (
    <div className="min-w-0">
      <div className="text-[10px] font-semibold uppercase tracking-wide text-neutral-500">{label}</div>
      <div className="flex items-center gap-2 truncate text-sm font-semibold text-neutral-900">
        {dot ? <span className={`h-2.5 w-2.5 shrink-0 rounded-full ring-3 ${DOT[dot] ?? 'bg-neutral-400 ring-neutral-400/25'}`} /> : null}
        <span className="truncate">{value}</span>
      </div>
    </div>
  );
}

export function StatGrid({ children }) {
  return <div className="mt-1.5 grid grid-cols-2 gap-x-4 gap-y-2 sm:grid-cols-4">{children}</div>;
}
