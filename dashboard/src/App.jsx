import { useCallback, useEffect, useRef, useState } from 'react';
import Stat, { StatGrid } from './components/Stat.jsx';
import ProcessList from './components/ProcessList.jsx';
import WorkerCard from './components/WorkerCard.jsx';
import HardwareCard from './components/HardwareCard.jsx';
import ApiClients from './components/ApiClients.jsx';
import RawStatus from './components/RawStatus.jsx';

const POLL_MS = 2000;

function healthLabel(check) {
  if (!check) return 'unknown';
  if (check.ok) return check.status ? `online ${check.status}` : 'online';
  return check.error || 'offline';
}

export default function App() {
  const [status, setStatus] = useState(null);
  const [failure, setFailure] = useState(null);
  const mounted = useRef(true);

  const refresh = useCallback(async () => {
    try {
      const response = await fetch('/status');
      const body = await response.json();
      if (!mounted.current) return;
      setStatus(body);
      setFailure(null);
    } catch (err) {
      if (!mounted.current) return;
      setFailure(err instanceof Error ? err.message : 'unknown_error');
    }
  }, []);

  useEffect(() => {
    mounted.current = true;
    refresh();
    const timer = setInterval(refresh, POLL_MS);
    return () => {
      mounted.current = false;
      clearInterval(timer);
    };
  }, [refresh]);

  const shellHealth = status?.endpoints?.shell?.health;
  const apiHealth = status?.endpoints?.api?.health;
  const scheduler = status?.scheduler ?? {};
  const agent = status?.agent ?? {};
  const workers = Array.isArray(agent.workers) ? agent.workers : [];
  const readyWorkers = workers.filter((worker) => worker.status === 'ready').length;
  const allReady = workers.length > 0 && readyWorkers === workers.length;

  return (
    <div className="min-h-screen bg-white px-4 pb-8 pt-3 text-[13px] text-neutral-900 antialiased">
      <header className="mb-3 flex items-baseline justify-between gap-4">
        <h1 className="text-base font-bold">System Status</h1>
        <div className="flex items-center gap-3">
          <span className="tabular-nums text-xs text-neutral-500">
            {failure ? failure : status ? new Date(status.timestamp).toLocaleTimeString() : 'checking'}
          </span>
          <button
            type="button"
            onClick={refresh}
            className="rounded-md border border-neutral-300 px-2.5 py-1 text-xs text-neutral-700 hover:bg-neutral-100 hover:text-neutral-900"
          >
            Refresh
          </button>
        </div>
      </header>

      <section className="mb-3 rounded-lg border border-neutral-200 bg-white px-3.5 py-2.5">
        <div className="mt-1.5 grid grid-cols-2 gap-x-4 gap-y-2 sm:grid-cols-3 lg:grid-cols-6">
          <Stat label="Shell singleton" value={healthLabel(shellHealth)} dot={shellHealth?.ok ? 'ok' : 'bad'} />
          <Stat label="API server" value={healthLabel(apiHealth)} dot={apiHealth?.ok ? 'ok' : 'bad'} />
          <Stat
            label="Workers ready"
            value={workers.length ? `${readyWorkers} of ${workers.length}` : 'unavailable'}
            dot={workers.length === 0 ? 'bad' : allReady ? 'ok' : 'warn'}
          />
          <Stat label="Active slots" value={`${scheduler.activeCount ?? 0} / ${scheduler.limits?.maxSlots ?? 0}`} />
          <Stat label="Queued" value={`${scheduler.queueLength ?? 0} / ${scheduler.limits?.maxQueue ?? 0}`} />
          <Stat label="API uptime" value={`${agent.uptime ?? 0}s`} />
        </div>
      </section>

      <main className="grid grid-cols-1 items-start gap-3 lg:grid-cols-3">
        <ProcessList registry={status?.registry} />
        <div className="flex min-w-0 flex-col gap-3">
          <WorkerCard workers={workers} scheduler={scheduler} agent={agent} />
          <HardwareCard modelConfig={status?.modelConfig} workers={workers} />
        </div>
        <ApiClients scheduler={scheduler} />
      </main>

      <RawStatus status={status ?? {}} />
    </div>
  );
}
