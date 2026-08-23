import { ChatClient } from '@serveinfer/chat-shared';

export default function App() {
  return (
    <div className="h-screen">
      <ChatClient
        id="chat_1"
        label="Chat 1"
      showPriority
      retry
      placeholder="Ask anything…"
      sendLabel="Submit"
      />
    </div>
  );
}
