import Card, { Heading } from './Card.jsx';
import Row, { Rows } from './Row.jsx';
import Stat, { StatGrid } from './Stat.jsx';

export default function WorkerCard({ workers, scheduler, agent }) {
  const ready = workers.filter((worker) => worker.status === 'ready').length;
  const limits = scheduler.limits ?? {};

  return (
    <Card title="Workers" tag={`${ready}/${workers.length} ready`}>
      <Rows>
        {workers.length ? (
          workers.map((worker) => {
            const isReady = worker.status === 'ready';
            const isBusy = worker.status === 'busy';
            return (
              <Row
                key={worker.id}
                name={`Worker ${worker.id}`}
                value={worker.status}
                dot={isReady ? 'ok' : isBusy ? 'none' : 'warn'}
                tone={isReady || isBusy ? 'none' : 'warn'}
              />
            );
          })
        ) : (
          <Row name="Workers" value="unavailable" dot="bad" tone="bad" />
        )}
      </Rows>

      <Heading title="Scheduler" tag="singleton shell" divider />
      <StatGrid>
        <Stat label="Active" value={`${scheduler.activeCount ?? 0}/${limits.maxSlots ?? 0}`} />
        <Stat label="Queue" value={`${scheduler.queueLength ?? 0}/${limits.maxQueue ?? 0}`} />
        <Stat label="Per MFE" value={limits.maxPerMfe ?? '-'} />
        <Stat label="Agent busy" value={agent.activeSlots ?? 0} />
      </StatGrid>
    </Card>
  );
}
