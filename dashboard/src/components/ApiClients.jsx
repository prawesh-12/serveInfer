import Card from './Card.jsx';
import Row, { Rows } from './Row.jsx';

export default function ApiClients({ scheduler }) {
  const active = Object.entries(scheduler.activeByMfe ?? {});
  const tag = active.length ? `${active.length} active` : `queue ${scheduler.queueLength ?? 0}`;

  return (
    <Card title="API clients" tag={tag}>
      <Rows>
        {active.length ? (
          active.map(([mfeId, count]) => <Row key={mfeId} name={mfeId} value={`${count} active`} dot="warn" />)
        ) : (
          <Row name="No client is holding a slot" value="idle" dot="ok" />
        )}
      </Rows>
    </Card>
  );
}
