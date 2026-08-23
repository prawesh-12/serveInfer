export default function RawStatus({ status }) {
  return (
    <details className="mt-3 rounded-lg border border-neutral-200 bg-white">
      <summary className="cursor-pointer select-none px-3.5 py-2 text-[11px] font-semibold uppercase tracking-wide text-neutral-500">
        Raw status
      </summary>
      <pre className="max-h-96 overflow-auto border-t border-neutral-200 px-3.5 py-3 font-mono text-[11px] leading-relaxed text-neutral-700">
        {JSON.stringify(status, null, 2)}
      </pre>
    </details>
  );
}
