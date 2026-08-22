const DOT = {
  ok: 'bg-emerald-500 ring-3 ring-emerald-500/25',
  warn: 'bg-amber-500 ring-3 ring-amber-500/25',
  bad: 'bg-red-500 ring-3 ring-red-500/25',
  none: 'bg-transparent',
};

const VALUE = {
  ok: 'text-neutral-600',
  warn: 'text-amber-700',
  bad: 'text-red-700',
  none: 'text-neutral-600',
};

export default function Row({ name, value, dot = 'none', tone = 'none' }) {
  return (
    <div className="grid grid-cols-[10px_minmax(0,1fr)_auto] items-center gap-2.5 border-t border-neutral-200 py-1 first:border-t-0">
      <span className={`h-2.5 w-2.5 rounded-full ${DOT[dot] ?? DOT.none}`} />
      <span className="truncate">{name}</span>
      <span className={`whitespace-nowrap text-xs tabular-nums ${VALUE[tone] ?? VALUE.none}`}>{value}</span>
    </div>
  );
}

export function GroupLabel({ children }) {
  return (
    <div className="mt-2.5 border-t border-neutral-200 pt-2 text-[10px] font-semibold uppercase tracking-wide text-neutral-500 first:mt-0 first:border-t-0 first:pt-0">
      {children}
    </div>
  );
}

export function Rows({ children }) {
  return <div className="mt-1">{children}</div>;
}
