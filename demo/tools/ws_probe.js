#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const WebSocket = require('../upstream/node_modules/ws');

const demoDir = path.resolve(__dirname, '..');
const ca = fs.readFileSync(path.join(demoDir, 'nginx', 'ca', 'ca.crt'));
const textPayload = `bronx-ws-${process.pid}-${Date.now()}`;
const binaryPayload = Buffer.from([0x00, 0x42, 0x7f, 0x80, 0xff]);
const pingPayload = Buffer.from('bronx-ping');

let upgradeStatus = 0;
let textEcho = false;
let binaryEcho = false;
let pong = false;
let finished = false;

const timer = setTimeout(() => finish(new Error('WebSocket probe timed out')), 8000);
const ws = new WebSocket('wss://gw.local/ws/echo', {
  ca,
  servername: 'gw.local',
  handshakeTimeout: 5000,
  perMessageDeflate: false,
  lookup: (_hostname, options, callback) => {
    if (options && options.all) {
      callback(null, [{ address: '127.0.0.1', family: 4 }]);
      return;
    }
    callback(null, '127.0.0.1', 4);
  }
});

function finish(error, closeCode = 0) {
  if (finished) return;
  finished = true;
  clearTimeout(timer);
  if (error) {
    console.error(error.message);
    process.exitCode = 1;
    if (ws.readyState === WebSocket.OPEN) ws.terminate();
    return;
  }
  console.log(JSON.stringify({
    url: 'wss://gw.local/ws/echo',
    upgrade: upgradeStatus,
    text_echo: textEcho,
    binary_echo: binaryEcho,
    pong,
    close_code: closeCode
  }));
  if (upgradeStatus !== 101 || !textEcho || !binaryEcho || !pong || closeCode !== 1000) {
    process.exitCode = 1;
  }
}

ws.on('upgrade', (response) => {
  upgradeStatus = response.statusCode;
});

ws.on('open', () => {
  ws.send(textPayload);
});

ws.on('message', (data, isBinary) => {
  if (!textEcho) {
    if (isBinary || data.toString() !== textPayload) {
      finish(new Error('text echo mismatch'));
      return;
    }
    textEcho = true;
    ws.send(binaryPayload, { binary: true });
    return;
  }
  if (!binaryEcho) {
    if (!isBinary || !Buffer.from(data).equals(binaryPayload)) {
      finish(new Error('binary echo mismatch'));
      return;
    }
    binaryEcho = true;
    ws.ping(pingPayload);
  }
});

ws.on('pong', (data) => {
  if (!Buffer.from(data).equals(pingPayload)) {
    finish(new Error('pong payload mismatch'));
    return;
  }
  pong = true;
  ws.close(1000, 'probe complete');
});

ws.on('error', (error) => finish(error));
ws.on('close', (code) => finish(null, code));
