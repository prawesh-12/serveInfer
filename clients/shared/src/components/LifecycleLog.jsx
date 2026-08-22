import { useCallback, useEffect, useRef, useState } from 'react';

const CHIPS = {
  queued: 'bg-white text-neutral-700 ring-neutral-200',
  started: 'bg-white text-neutral-700 ring-neutral-200',
  token: 'bg-neutral-100 text-neutral-500 ring-neutral-200',
  done: 'bg-white text-neutral-700 ring-neutral-200',
  error: 'bg-red-50 text-red-600 ring-red-200',
  cancelled: 'bg-neutral-100 text-neutral-700 ring-neutral-300',
  timeout: 'bg-white text-neutral-700 ring-neutral-200',
};

const CHIP_FALLBACK = 'bg-neutral-100 text-neutral-500 ring-neutral-200';

function formatTime(at) {
  const date = new Date(at);
  if (Number.isNaN(date.getTime())) return '--:--:--';
  return date.toLocaleTimeString('en-GB', { hour12: false });
}

function compactDetail(detail) {
  if (detail === null || detail === undefined) return '';
  if (typeof detail === 'string') return detail;
  if (typeof detail !== 'object') return String(detail);
  const parts = Object.entries(detail).map(([key, value]) => {
    const rendered = value === null || typeof value === 'object' ? JSON.stringify(value) : String(value);
    return `${key}=${rendered}`;
  });
  const line = parts.join(' ');
  return line.length > 160 ? `${line.slice(0, 157)}…` : line;
}

export function LifecycleLog({ events = [] }) {
  const scrollRef = useRef(null);
  const [pinned, setPinned] = useState(true);

  const handleScroll = useCallback(() => {
    const node = scrollRef.current;
    if (!node) return;
    setPinned(node.scrollHeight - node.scrollTop - node.clientHeight < 32);
  }, []);

  useEffect(() => {
    const node = scrollRef.current;
    if (!node || !pinned) return;
    node.scrollTop = node.scrollHeight;
  }, [events, pinned]);

  return (
    <div className="flex h-full min-h-0 flex-col overflow-hidden border-t border-neutral-200 bg-neutral-50">
      <div className="flex shrink-0 items-center justify-between border-b border-neutral-200 px-2 py-1">
        <span className="font-mono text-[10px] tracking-widest text-neutral-500 uppercase">lifecycle</span>
        <span className="font-mono text-[10px] text-neutral-400">{events.length}</span>
      </div>
      <div
        ref={scrollRef}
        onScroll={handleScroll}
        className="min-h-0 flex-1 overflow-x-hidden overflow-y-auto px-2 py-1 font-mono text-[10px] leading-snug"
      >
        {events.length === 0 ? (
          <p className="py-3 text-center text-neutral-700">no events</p>
        ) : (
          events.map((event) => (
            <div key={event.id} className="flex items-start gap-1.5 border-b border-neutral-900/80 py-0.5 last:border-0">
              <span className="shrink-0 text-neutral-400 tabular-nums">{formatTime(event.at)}</span>
              <span
                className={`shrink-0 rounded px-1 ring-1 ring-inset ${CHIPS[event.type] || CHIP_FALLBACK}`}
              >
                {event.type}
              </span>
              <span className="min-w-0 flex-1 break-all text-neutral-500">{compactDetail(event.detail)}</span>
            </div>
          ))
        )}
      </div>
    </div>
  );
}

export default LifecycleLog;
