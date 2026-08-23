import { useCallback, useState } from 'react';
import { ChatClient } from '@serveinfer/chat-shared';
import SummaryBar from './SummaryBar.jsx';

const CLIENTS = [
  { id: 'chat_1', label: 'Chat 1', showPriority: true, retry: true, sendLabel: 'Submit' },
  { id: 'chat_2', label: 'Chat 2', multiline: true, showTokens: true, sendLabel: 'Start stream' },
  { id: 'chat_3', label: 'Chat 3' },
  { id: 'chat_4', label: 'Chat 4' },
  { id: 'chat_5', label: 'Chat 5' },
];

export default function App() {
  const [states, setStates] = useState({});

  const onStateChange = useCallback((id, state) => {
    setStates((prev) => ({ ...prev, [id]: state }));
  }, []);

  return (
    <div className="flex h-screen min-h-0 flex-col bg-white">
      <SummaryBar clients={CLIENTS} states={states} />
      <div className="grid min-h-0 flex-1 grid-cols-1 divide-neutral-200 md:grid-cols-2 xl:grid-cols-5 xl:divide-x">
        {CLIENTS.map((client) => (
          <div key={client.id} className="min-h-0 overflow-hidden">
            <ChatClient {...client} onStateChange={onStateChange} />
          </div>
        ))}
      </div>
    </div>
  );
}
