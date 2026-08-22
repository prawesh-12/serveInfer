import Card from './Card.jsx';
import Row, { Rows } from './Row.jsx';
import Stat, { StatGrid } from './Stat.jsx';

function gb(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) return '-';
  return `${(bytes / 1024 ** 3).toFixed(1)} GB`;
}

function mb(value) {
  if (!Number.isFinite(value) || value <= 0) return '-';
  return `${value} MB`;
}

function usage(freeBytes, totalBytes) {
  if (!Number.isFinite(totalBytes) || totalBytes <= 0) return '-';
  const used = Math.max(0, totalBytes - (freeBytes || 0));
  return `${gb(used)} used · ${gb(freeBytes)} free · ${Math.round((used / totalBytes) * 100)}%`;
}

function workerTierRow(assignment, live) {
  const planned = assignment.backend ?? '-';
  const running = live?.device ?? null;
  const name = `Worker ${assignment.workerId}`;

  if (!running) return <Row key={name} name={name} value={`planned ${planned} · no request yet`} dot="warn" tone="warn" />;
  if (live.degraded) {
    return <Row key={name} name={name} value={`${planned} → ${running} · ${live.degradedReason || 'degraded'}`} dot="bad" tone="bad" />;
  }
  if (running !== planned) return <Row key={name} name={name} value={`planned ${planned} · running ${running}`} dot="warn" tone="warn" />;
  return <Row key={name} name={name} value={running} dot="ok" />;
}

export default function HardwareCard({ modelConfig, workers }) {
  const hardware = modelConfig?.hardware;

  if (!hardware) {
    return (
      <Card title="Hardware" tag="no probe">
        <Rows>
          <Row name="No discovery on file" value="start the backend to probe" dot="warn" tone="warn" />
        </Rows>
      </Card>
    );
  }

  const capacity = modelConfig.capacity;
  const assignments = Array.isArray(modelConfig.assignments) ? modelConfig.assignments : [];
  const gpus = Array.isArray(hardware.gpus) ? hardware.gpus : [];
  const liveById = new Map(workers.map((worker) => [worker.id, worker]));
  const tag = hardware.probeOk ? (gpus.length ? `${gpus.length} gpu${gpus.length > 1 ? 's' : ''}` : 'cpu only') : 'probe failed';

  return (
    <Card title="Hardware" tag={tag}>
      <StatGrid>
        <Stat label="Probe" value={hardware.probeOk ? 'ok' : 'failed'} dot={hardware.probeOk ? 'ok' : 'bad'} />
        <Stat label="Accelerator" value={gpus.length ? capacity?.gpuName || gpus[0].name : 'none'} />
        <Stat label="RAM free" value={gb(hardware.ramAvailableBytes)} />
        <Stat label="Placed" value={`${modelConfig.workerCount ?? '-'} / ${modelConfig.configuredWorkerCount ?? '-'}`} />
      </StatGrid>

      <Rows>
        {gpus.length ? (
          gpus.map((gpu) => (
            <div key={gpu.name}>
              <Row name={gpu.description || gpu.name} value={usage(gpu.freeBytes, gpu.totalBytes)} dot="ok" />
              <Row name="VRAM total" value={gb(gpu.totalBytes)} />
              <Row name="VRAM free" value={gb(gpu.freeBytes)} />
            </div>
          ))
        ) : (
          <Row name="No GPU found" value={hardware.note || 'cpu only'} dot="warn" tone="warn" />
        )}

        <Row name="Host RAM" value={usage(hardware.ramAvailableBytes, hardware.ramTotalBytes)} dot="ok" />
        <Row name="RAM total" value={gb(hardware.ramTotalBytes)} />
        <Row name="RAM free" value={gb(hardware.ramAvailableBytes)} />

        {capacity ? (
          <div>
            {gpus.length ? <Row name="GPU budget" value={`${mb(capacity.usableGpuMb)} usable of ${mb(capacity.freeVramMb)} free`} /> : null}
            <Row name="CPU budget" value={`${mb(capacity.usableRamMb)} usable of ${mb(capacity.availableRamMb)} available`} />
            <Row name="Capacity" value={`${capacity.gpuWorkerCapacity} gpu + ${capacity.cpuWorkerCapacity} cpu worker(s)`} />
            {capacity.gpuReason ? <Row name="GPU plan" value={capacity.gpuReason} /> : null}
            {capacity.cpuReason ? <Row name="CPU plan" value={capacity.cpuReason} /> : null}
          </div>
        ) : null}

        {assignments.map((assignment) => workerTierRow(assignment, liveById.get(assignment.workerId)))}
      </Rows>
    </Card>
  );
}
