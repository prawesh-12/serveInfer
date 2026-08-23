import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

const target = `http://127.0.0.1:${process.env.DASHBOARD_PORT || 3001}`;

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    host: '127.0.0.1',
    port: Number(process.env.DASHBOARD_DEV_PORT || 5174),
    proxy: {
      '/status': target,
      '/dashboard-config.js': target,
    },
  },
});
