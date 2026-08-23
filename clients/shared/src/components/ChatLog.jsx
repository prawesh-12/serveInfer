import { useCallback, useEffect, useRef, useState } from 'react';

const BUBBLES = {
  user: 'ml-4 border-neutral-900 bg-neutral-900 text-white',
  assistant: 'mr-4 border-neutral-300 bg-neutral-100 text-neutral-800',
  error: 'mr-4 border-red-200 bg-red-50 text-red-700',
};

const LABELS = { user: 'you', assistant: 'model', error: 'error' };

export function ChatLog({ messages = [], isStreaming = false }) {
  const scrollRef = useRef(null);
  const [pinned, setPinned] = useState(true);

  const handleScroll = useCallback(() => {
    const node = scrollRef.current;
    if (!node) return;
    // treat "close enough to the bottom" as pinned, so a nudge does not detach the follow
    setPinned(node.scrollHeight - node.scrollTop - node.clientHeight < 48);
  }, []);

  useEffect(() => {
    const node = scrollRef.current;
    if (!node || !pinned) return;
    node.scrollTop = node.scrollHeight;
  }, [messages, isStreaming, pinned]);

  return (
    <div className="relative flex h-full min-h-0 flex-col overflow-hidden">
      <div
        ref={scrollRef}
        onScroll={handleScroll}
        className="min-h-0 flex-1 space-y-2 overflow-x-hidden overflow-y-auto p-2"
      >
        {messages.length === 0 ? (
          <p className="px-1 py-6 text-center text-[11px] text-neutral-400">No messages yet.</p>
        ) : (
          messages.map((message) => {
            const role = message.role === 'user' || message.role === 'error' ? message.role : 'assistant';
            return (
              <div key={message.id} className={`rounded-lg border px-2 py-1.5 ${BUBBLES[role]}`}>
                <div className="mb-0.5 text-[10px] font-semibold tracking-widest text-neutral-500 uppercase">
                  {LABELS[role]}
                </div>
                <p className="text-xs leading-relaxed break-words whitespace-pre-wrap">{message.text}</p>
                {role === 'assistant' && (message.device || message.degraded) && (
                  <div className="mt-1.5 flex flex-wrap items-center gap-1">
                    {message.device && (
                      <span className="rounded bg-neutral-100 px-1.5 py-0.5 font-mono text-[10px] text-neutral-600">
                        {message.device}
                      </span>
                    )}
                    {message.degraded && (
                      <span
                        title={message.degradedReason || 'degraded'}
                        className="rounded bg-white/70 px-1.5 py-0.5 text-[10px] font-medium text-neutral-700"
                      >
                        degraded{message.degradedReason ? `: ${message.degradedReason}` : ''}
                      </span>
                    )}
                  </div>
                )}
              </div>
            );
          })
        )}
        {isStreaming && (
          <div className="flex items-center gap-1.5 px-1 text-[10px] tracking-widest text-neutral-500 uppercase">
            <span className="size-1.5 animate-pulse rounded-full bg-neutral-400" />
            streaming
          </div>
        )}
      </div>
      {!pinned && messages.length > 0 && (
        <button
          type="button"
          onClick={() => setPinned(true)}
          className="absolute inset-x-0 bottom-1.5 mx-auto w-fit rounded-full border border-neutral-300 bg-neutral-100/95 px-2.5 py-1 text-[10px] font-medium text-neutral-800 shadow-lg hover:bg-neutral-100"
        >
          Jump to latest ↓
        </button>
      )}
    </div>
  );
}

export default ChatLog;
