import { ChatClient } from '@serveinfer/chat-shared';

export default function App() {
  return (
    <div className="h-screen">
      <ChatClient
        id="chat_2"
        label="Chat 2"
      multiline
      showTokens
      placeholder="Paste a transcript or a long prompt…"
      sendLabel="Start stream"
      />
    </div>
  );
}
