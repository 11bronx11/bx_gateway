#!/usr/bin/env node
'use strict';

const http = require('node:http');
const { WebSocketServer } = require('ws');

const host = process.env.WS_HOST || '127.0.0.1';
const port = Number(process.env.WS_PORT || 3003);

const server = http.createServer((req, res) => {
  if (req.url === '/healthz') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, service: 'bronx-ws-echo' }));
    return;
  }
  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'not found' }));
});

const wss = new WebSocketServer({ noServer: true, maxPayload: 1024 * 1024 });

server.on('upgrade', (req, socket, head) => {
  if (req.url !== '/echo') {
    socket.end('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n');
    return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    wss.emit('connection', ws, req);
  });
});

wss.on('connection', (ws) => {
  ws.on('message', (data, isBinary) => {
    ws.send(data, { binary: isBinary });
  });
});

let stopping = false;
function stop() {
  if (stopping) return;
  stopping = true;
  for (const client of wss.clients) client.close(1001, 'server shutdown');
  wss.close(() => server.close(() => process.exit(0)));
  setTimeout(() => process.exit(1), 3000).unref();
}

process.on('SIGINT', stop);
process.on('SIGTERM', stop);

server.listen(port, host, () => {
  console.log(`ws echo ready on ${host}:${port}`);
});
