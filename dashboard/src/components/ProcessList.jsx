import Card from './Card.jsx';
import Row, { GroupLabel, Rows } from './Row.jsx';

const TIER_LABEL = { backend: 'Backend', clients: 'Clients', dashboard: 'Dashboard', other: 'Other' };
const TIER_ORDER = ['backend', 'clients', 'dashboard', 'other'];

export default function ProcessList({ registry }) {
  const running = registry?.processes ?? [];
  const stale = registry?.stale ?? [];
  const tag = stale.length ? `${running.length} up, ${stale.length} stale` : `${running.length} registered`;

  if (running.length === 0 && stale.length === 0) {
    return (
      <Card title="Processes" tag={tag}>
        <Rows>
          <Row name="Nothing registered" value={registry?.stateDir ?? 'no state dir'} dot="bad" tone="bad" />
        </Rows>
      </Card>
    );
  }

  const byTier = new Map();
  for (const proc of running) {
    if (!byTier.has(proc.tier)) byTier.set(proc.tier, []);
    byTier.get(proc.tier).push(proc);
  }

  return (
    <Card title="Processes" tag={tag}>
      <Rows>
        {TIER_ORDER.filter((tier) => byTier.get(tier)?.length).map((tier) => (
          <div key={tier}>
            <GroupLabel>{TIER_LABEL[tier] ?? tier}</GroupLabel>
            {byTier.get(tier).map((proc) => (
              <Row key={proc.name} name={proc.label} value={`${proc.pid} · ${proc.uptime}`} dot="ok" />
            ))}
          </div>
        ))}
        {stale.map((proc) => (
          <Row key={proc.name} name={proc.label} value={`pid ${proc.pid} gone, stale pidfile`} dot="bad" tone="bad" />
        ))}
      </Rows>
    </Card>
  );
}
