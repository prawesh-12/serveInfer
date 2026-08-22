'use strict';

const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');

const port = Number(process.env.CLIENT_PORT || 5001);
const shellApiBase = process.env.SHELL_API_BASE || 'http://127.0.0.1:3000';
const publicDir = path.join(__dirname, 'public');

const retryPolicy = {
  attempts: Number(process.env.CLIENT_RETRY_ATTEMPTS || 3),
  baseMs: Number(process.env.CLIENT_RETRY_BASE_MS || 500),
  maxMs: Number(process.env.CLIENT_RETRY_MAX_MS || 8000),
};

const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
};

function send(res, status, body, contentType = 'text/plain; charset=utf-8') {
  res.writeHead(status, { 'Content-Type': contentType });
  res.end(body);
}

function serveStatic(req, res) {
  const requestUrl = new URL(req.url, `http://${req.headers.host || '127.0.0.1'}`);
  if (requestUrl.pathname === '/config.js') {
    send(
      res,
      200,
      `window.MFE_CONFIG = ${JSON.stringify({ shellApiBase, retry: retryPolicy })};\n`,
      'text/javascript; charset=utf-8'
    );
    return;
  }

  const pathname = requestUrl.pathname === '/' ? '/index.html' : requestUrl.pathname;
  const filePath = path.normalize(path.join(publicDir, pathname));
  if (!filePath.startsWith(publicDir)) {
    send(res, 403, 'forbidden');
    return;
  }

  fs.readFile(filePath, (err, content) => {
    if (err) {
      send(res, 404, 'not_found');
      return;
    }
    send(res, 200, content, contentTypes[path.extname(filePath)] || 'application/octet-stream');
  });
}

http.createServer(serveStatic).listen(port, '127.0.0.1', () => {
  console.log(`[meeting-mfe] listening on http://127.0.0.1:${port}`);
  console.log(`[meeting-mfe] shell API: ${shellApiBase}`);
});
