import { StatusDot } from '@serveinfer/chat-shared';

const BUSY = new Set(['queued', 'streaming']);

function detailFor(state) {
  if (!state) return 'starting';
  if (state.connection === 'offline') return 'shell offline';
  if (state.status === 'queued') {
    const eta = state.etaSeconds !== null && state.etaSeconds !== undefined ? ` · ~${state.etaSeconds}s` : '';
    return `queued #${state.queuePosition ?? '?'}${eta}`;
  }
  if (state.status === 'streaming') {
    const many = state.inFlight > 1 ? ` · ${state.inFlight} in flight` : '';
    return `streaming · ${state.tokenCount} tokens${many}`;
  }
  if (state.status === 'error') return state.error?.code ?? 'error';
  if (state.status === 'done') return `done on ${state.device ?? 'unknown'}`;
  if (state.status === 'cancelled') return 'cancelled';
  return state.device ? `idle · last on ${state.device}` : 'idle';
}

export default function SummaryBar({ clients, states }) {
  const list = clients.map((client) => ({ ...client, state: states[client.id] }));
  const running = list.filter((c) => c.state?.status === 'streaming').length;
  const queued = list.filter((c) => c.state?.status === 'queued').length;
  const idle = list.filter((c) => !BUSY.has(c.state?.status)).length;
  const offline = list.filter((c) => c.state?.connection === 'offline').length;

  const byDevice = new Map();
  for (const c of list) {
    if (c.state?.status === 'streaming' && c.state.device) {
      byDevice.set(c.state.device, (byDevice.get(c.state.device) ?? 0) + 1);
    }
  }
  const devices = [...byDevice.entries()].map(([name, n]) => `${n}×${name}`).join(' · ');

  return (
    <header className="shrink-0 border-b border-neutral-200 bg-neutral-50">
      <div className="flex flex-wrap items-baseline gap-x-4 gap-y-1 px-3 py-1.5">
        <span className="text-xs font-bold text-neutral-900">Chat clients</span>
        <span className="text-[11px] tabular-nums text-neutral-600">
          {running} running · {queued} queued · {idle} idle
          {offline ? ` · ${offline} offline` : ''}
          {devices ? ` · ${devices}` : ''}
        </span>
      </div>
      <div className="grid grid-cols-1 divide-neutral-200 border-t border-neutral-200 md:grid-cols-2 xl:grid-cols-5 xl:divide-x">
        {list.map((client) => (
          <div key={client.id} className="flex min-w-0 items-center gap-2 px-3 py-1.5">
            <StatusDot state={client.state?.status ?? 'idle'} label="" />
            <span className="shrink-0 text-[11px] font-semibold text-neutral-900">{client.label}</span>
            <span className="truncate text-[11px] text-neutral-600">{detailFor(client.state)}</span>
          </div>
        ))}
      </div>
    </header>
  );
}
