import { useState } from 'react';

const PRIMARY = 'rounded-md bg-neutral-900 px-3 py-1.5 text-xs font-semibold text-white transition-colors hover:bg-neutral-700 disabled:cursor-not-allowed disabled:bg-neutral-300 disabled:text-neutral-500';
const SECONDARY = 'rounded-md border border-neutral-300 bg-white px-2.5 py-1.5 text-[11px] font-medium text-neutral-700 transition-colors hover:bg-neutral-100 disabled:cursor-not-allowed disabled:border-neutral-200 disabled:text-neutral-400 disabled:hover:bg-white';
const FIELD = 'w-full rounded-md border border-neutral-300 bg-white px-2.5 py-1.5 text-xs text-neutral-900 placeholder:text-neutral-400 focus:border-neutral-900 focus:ring-1 focus:ring-neutral-900/20 focus:outline-none disabled:cursor-not-allowed disabled:bg-neutral-100 disabled:text-neutral-400';

export function ChatComposer({
  onSend,
  onBurst,
  onCancel,
  onClear,
  isBusy = false,
  disabled = false,
  multiline = false,
  burstCount = 5,
  showPriority = false,
  placeholder = 'Prompt…',
  sendLabel = 'Send',
}) {
  const [value, setValue] = useState('');
  const trimmed = value.trim();
  const canSend = !disabled && !isBusy && trimmed.length > 0;

  function fire(priority) {
    if (!canSend) return;
    onSend?.(trimmed, { priority });
    setValue('');
  }

  function submit(event) {
    event.preventDefault();
    fire(showPriority ? 'high' : undefined);
  }

  function onKeyDown(event) {
    if (multiline && event.key === 'Enter' && (event.metaKey || event.ctrlKey)) submit(event);
  }

  return (
    <form onSubmit={submit} className="flex shrink-0 flex-col gap-2 border-b border-neutral-200 bg-neutral-50 p-2 @3xl:p-3">
      {multiline ? (
        <textarea
          value={value}
          onChange={(event) => setValue(event.target.value)}
          onKeyDown={onKeyDown}
          disabled={disabled}
          placeholder={disabled ? 'unavailable' : placeholder}
          aria-label="Prompt"
          className={`${FIELD} h-20 resize-y font-mono leading-relaxed @3xl:h-28`}
        />
      ) : (
        <input
          type="text"
          value={value}
          onChange={(event) => setValue(event.target.value)}
          disabled={disabled}
          placeholder={disabled ? 'unavailable' : placeholder}
          aria-label="Prompt"
          className={FIELD}
        />
      )}

      <div className="flex flex-wrap items-center gap-2">
        <button type="submit" disabled={!canSend} className={PRIMARY}>
          {isBusy ? '…' : showPriority ? `${sendLabel} HIGH` : sendLabel}
        </button>
        {showPriority ? (
          <>
            <button type="button" onClick={() => fire('low')} disabled={!canSend} className={SECONDARY}>
              Prefetch LOW
            </button>
            <button
              type="button"
              onClick={() => canSend && (onBurst?.(trimmed, burstCount), setValue(''))}
              disabled={!canSend}
              className={SECONDARY}
            >
              Burst LOW x{burstCount}
            </button>
          </>
        ) : null}
      </div>

      <div className="flex flex-wrap items-center gap-2">
        <button type="button" onClick={() => onCancel?.()} disabled={!isBusy} className={SECONDARY}>
          Cancel
        </button>
        <button type="button" onClick={() => onClear?.()} className={SECONDARY}>
          Clear
        </button>
        {multiline ? <span className="text-[11px] text-neutral-400">Ctrl+Enter to send</span> : null}
      </div>
    </form>
  );
}

export default ChatComposer;
