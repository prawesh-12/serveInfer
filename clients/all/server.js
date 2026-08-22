import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';

const port = Number(process.env.CLIENT_PORT || 5000);
const shellApiBase = process.env.SHELL_API_BASE || 'http://127.0.0.1:3000';
const distDir = path.join(import.meta.dirname, 'dist');

const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
};

function send(res, status, body, contentType = 'text/plain; charset=utf-8') {
  res.writeHead(status, { 'Content-Type': contentType });
  res.end(body);
}

function sendFile(res, filePath) {
  fs.readFile(filePath, (err, content) => {
    if (err) {
      send(res, 404, 'not_found');
      return;
    }
    send(res, 200, content, contentTypes[path.extname(filePath)] || 'application/octet-stream');
  });
}

function serveStatic(req, res) {
  const requestUrl = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
  if (requestUrl.pathname === '/config.js') {
    send(
      res,
      200,
      `window.MFE_CONFIG = ${JSON.stringify({ shellApiBase })};\n`,
      'text/javascript; charset=utf-8'
    );
    return;
  }

  if (!fs.existsSync(distDir)) {
    send(res, 503, 'chat-dashboard is not built: run "pnpm build" in clients/all\n');
    return;
  }

  const pathname = requestUrl.pathname === '/' ? '/index.html' : requestUrl.pathname;
  const filePath = path.normalize(path.join(distDir, pathname));
  if (!filePath.startsWith(distDir)) {
    send(res, 403, 'forbidden');
    return;
  }

  fs.stat(filePath, (err, stats) => {
    // Unknown paths are SPA routes, so the built index.html has to answer them.
    if (err || !stats.isFile()) {
      sendFile(res, path.join(distDir, 'index.html'));
      return;
    }
    sendFile(res, filePath);
  });
}

http.createServer(serveStatic).listen(port, '127.0.0.1', () => {
  console.log(`[all] listening on http://127.0.0.1:${port}`);
  console.log(`[all] shell API: ${shellApiBase}`);
});
