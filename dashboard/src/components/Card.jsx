export default function Card({ title, tag, children }) {
  return (
    <section className="flex min-w-0 flex-col rounded-lg border border-neutral-200 bg-white px-3.5 py-2.5">
      <Heading title={title} tag={tag} />
      {children}
    </section>
  );
}

export function Heading({ title, tag, divider = false }) {
  return (
    <h2
      className={`flex items-baseline justify-between gap-2 text-[11px] font-semibold uppercase tracking-wide text-neutral-500 ${
        divider ? 'mt-3.5 border-t border-neutral-200 pt-2.5' : ''
      }`}
    >
      <span>{title}</span>
      {tag ? <span className="text-[11px] font-normal normal-case tracking-normal tabular-nums text-neutral-500">{tag}</span> : null}
    </h2>
  );
}
