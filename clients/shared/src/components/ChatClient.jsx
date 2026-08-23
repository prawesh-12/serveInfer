import { useEffect } from 'react';
import { useChatClient } from '../hooks/useChatClient.js';
import ChatComposer from './ChatComposer.jsx';
import ChatLog from './ChatLog.jsx';
import LifecycleLog from './LifecycleLog.jsx';
import StatusDot from './StatusDot.jsx';

export function ChatClient({
  id,
  label,
  priority = 'high',
  retry = false,
  multiline = false,
  showPriority = false,
  showTokens = false,
  placeholder,
  sendLabel,
  onStateChange,
}) {
  const client = useChatClient({ id, priority, retry });
  const { status, connection, device, inFlight, tokenCount, queuePosition, etaSeconds, error } = client;

  useEffect(() => {
    onStateChange?.(id, { status, connection, device, inFlight, tokenCount, queuePosition, etaSeconds, error });
  }, [id, onStateChange, status, connection, device, inFlight, tokenCount, queuePosition, etaSeconds, error]);


  return (
    <div className="@container flex h-full min-h-0 flex-col overflow-hidden bg-white text-neutral-900 antialiased">
      <header className="flex shrink-0 flex-wrap items-center justify-between gap-3 border-b border-neutral-200 bg-neutral-50 px-4 py-2.5">
        <div className="flex items-baseline gap-2.5">
          <h1 className="text-sm font-bold text-neutral-900">{label}</h1>
          <span className="font-mono text-[11px] text-neutral-500">{id}</span>
        </div>
        <div className="flex items-center gap-3">
          {showTokens ? <span className="text-[11px] tabular-nums text-neutral-500">{client.tokenCount} tokens</span> : null}
          {client.inFlight > 1 ? (
            <span className="text-[11px] tabular-nums text-neutral-500">{client.inFlight} in flight</span>
          ) : null}
          {client.queuePosition !== null ? (
            <span className="text-[11px] tabular-nums text-neutral-500">
              queued #{client.queuePosition}
              {client.etaSeconds !== null ? ` · ~${client.etaSeconds}s` : ''}
            </span>
          ) : null}
          <StatusDot state={client.status} label={client.status} />
          <StatusDot state={client.connection} label={client.connection} />
        </div>
      </header>

      <ChatComposer
        onSend={client.send}
        onBurst={client.burst}
        onCancel={client.cancel}
        onClear={client.clear}
        isBusy={client.isBusy}
        disabled={client.connection === 'offline'}
        multiline={multiline}
        showPriority={showPriority}
        placeholder={placeholder}
        sendLabel={sendLabel}
      />

      {client.error ? (
        <div className="shrink-0 border-b border-red-200 bg-red-50/40 px-4 py-1.5 text-[11px] text-red-600">
          {client.error.code}: {client.error.message}
        </div>
      ) : null}

      <main className="grid min-h-0 flex-1 grid-cols-1 grid-rows-[minmax(0,3fr)_minmax(0,2fr)] gap-2 p-2 @3xl:grid-cols-[minmax(0,3fr)_minmax(0,2fr)] @3xl:grid-rows-1 @3xl:gap-3 @3xl:p-3">
        <section className="flex min-h-0 flex-col overflow-hidden rounded-xl border border-neutral-200 bg-white">
          <h2 className="shrink-0 border-b border-neutral-200 px-3 py-2 text-[11px] font-semibold uppercase tracking-wide text-neutral-500">
            Conversation
          </h2>
          <ChatLog messages={client.messages} isStreaming={client.status === 'streaming'} />
        </section>

        <section className="flex min-h-0 flex-col overflow-hidden rounded-xl border border-neutral-200 bg-white/60">
          <LifecycleLog events={client.events} />
        </section>
      </main>
    </div>
  );
}

export default ChatClient;
