#!/usr/bin/env node
// Reference encoder for the "hncfg" block (see src/config/Config.h).
// The web app ships the same algorithm; catalog/fixtures/ pins the exact bytes so
// firmware, web and this script can be tested against each other.
//
//   node tools/encode-config.mjs <config.json> <out.bin> [--pad 8192]

import { readFileSync, writeFileSync } from 'node:fs';

const MAGIC = [0x48, 0x4e, 0x43, 0x31]; // "HNC1"
const SCHEMA = 1;
const HEADER = 16;

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(bytes) {
  let crc = 0xffffffff;
  for (const b of bytes) crc = CRC_TABLE[(crc ^ b) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

/** Encodes a config object into a block; `pad` fills the rest with 0xFF. */
export function encodeConfig(config, pad = 0) {
  const payload = new TextEncoder().encode(JSON.stringify(config));
  const size = Math.max(HEADER + payload.length, pad);
  if (HEADER + payload.length > 8192) throw new Error('config too large');
  const block = new Uint8Array(size).fill(0xff);
  const view = new DataView(block.buffer);
  block.set(MAGIC, 0);
  view.setUint16(4, SCHEMA, true);
  view.setUint16(6, 0, true);
  view.setUint32(8, payload.length, true);
  view.setUint32(12, crc32(payload), true);
  block.set(payload, HEADER);
  return block;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const [input, output] = process.argv.slice(2);
  const padIdx = process.argv.indexOf('--pad');
  const pad = padIdx > 0 ? Number(process.argv[padIdx + 1]) : 0;
  const block = encodeConfig(JSON.parse(readFileSync(input, 'utf8')), pad);
  writeFileSync(output, block);
  console.log(`${output}: ${block.length} bytes, crc ${crc32(block.subarray(HEADER, HEADER + new DataView(block.buffer).getUint32(8, true))).toString(16)}`);
}
