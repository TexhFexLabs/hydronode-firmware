#!/usr/bin/env node
// Validates catalog/*.json against each other and against platformio.ini, and
// writes the merged catalog the web app consumes (dist/catalog.json).
//
//   node tools/validate-catalog.mjs            validate only
//   node tools/validate-catalog.mjs --out dist write dist/catalog.json too

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => JSON.parse(readFileSync(join(root, p), 'utf8'));

const boards = read('catalog/boards.json');
const drivers = read('catalog/drivers.json');
const sleep = read('catalog/sleep-modes.json');
const libs = read('catalog/libraries.json');

const errors = [];
const fail = (msg) => errors.push(msg);
const TYPE_RE = /^[A-Z][A-Z0-9_]{0,63}$/;

// --- libraries ---------------------------------------------------------------
const libByName = new Map();
for (const lib of libs.libraries) {
  if (libByName.has(lib.name)) fail(`library ${lib.name}: duplicate`);
  libByName.set(lib.name, lib);
  if (!libs.allowedLicenses.includes(lib.license)) fail(`library ${lib.name}: license ${lib.license} not allowed`);
  if (!/^\d+\.\d+\.\d+$/.test(lib.version)) fail(`library ${lib.name}: version must be exact, got ${lib.version}`);
}
for (const name of libs.base) if (!libByName.has(name)) fail(`base library ${name} missing in libraries`);

// platformio.ini lib_deps must match libraries.json exactly, in both directions.
const ini = readFileSync(join(root, 'platformio.ini'), 'utf8');
const envBlock = ini.split(/^\[esp\]\s*$/m)[1]?.split(/^\[/m)[0] ?? '';
// lib_deps continues on indented lines until the next key.
const depLines = [];
for (const line of (envBlock.split(/^lib_deps\s*=\s*$/m)[1] ?? '').split('\n').slice(1)) {
  if (!/^\s+\S/.test(line)) break;
  depLines.push(line.trim());
}
const iniDeps = new Map(depLines.map((l) => {
  const at = l.lastIndexOf('@');
  return [l.slice(0, at), l.slice(at + 1)];
}));
// [env:esp8266] may differ only in HydroNode-Library: ESP8266 support starts with 1.4.0, which
// is fetched from its GitHub release tag (the PlatformIO registry still lists 1.3.0).
const esp8266Block = ini.split(/^\[env:esp8266\]\s*$/m)[1]?.split(/^\[/m)[0] ?? '';
if (esp8266Block) {
  const lines = [];
  for (const line of (esp8266Block.split(/^lib_deps\s*=\s*$/m)[1] ?? '').split('\n').slice(1)) {
    if (!/^\s+\S/.test(line)) break;
    lines.push(line.trim());
  }
  for (const dep of lines) {
    if (dep.startsWith('https://github.com/TexhFexLabs/HydroNode-Library.git#')) {
      if (!/#\d+\.\d+\.\d+$/.test(dep)) fail(`[env:esp8266]: ${dep} must pin a release tag`);
      continue;
    }
    if (!depLines.includes(dep)) fail(`[env:esp8266]: ${dep} differs from [esp]`);
  }
  for (const dep of depLines) {
    if (!dep.startsWith('texhfexlabs/HydroNode-Library') && !lines.includes(dep)) fail(`[env:esp8266]: ${dep} missing`);
  }
}
for (const lib of libs.libraries) {
  if (iniDeps.get(lib.pio) !== lib.version) fail(`platformio.ini: ${lib.pio}@${lib.version} expected, found ${iniDeps.get(lib.pio) ?? 'nothing'}`);
}
for (const [pio] of iniDeps) {
  if (!libs.libraries.some((l) => l.pio === pio)) fail(`platformio.ini: ${pio} is not listed in catalog/libraries.json`);
}

// --- sleep modes ---------------------------------------------------------------
const modeIds = new Set(sleep.modes.map((m) => m.id));

// --- boards ---------------------------------------------------------------------
const boardIds = new Set();
for (const [id, fam] of Object.entries(boards.families)) {
  for (const key of ['inputOnly', 'adc', 'strapping', 'serial', 'wakePins']) {
    for (const pin of fam[key]) {
      if (!fam.gpios.includes(pin)) fail(`family ${id}: ${key} pin ${pin} is not in gpios`);
    }
  }
  for (const pin of fam.usb) if (fam.gpios.includes(pin)) fail(`family ${id}: USB pin ${pin} must not be offered as gpio`);
  for (const m of fam.sleepModes) if (!modeIds.has(m)) fail(`family ${id}: unknown sleep mode ${m}`);
  if (![1, 2].includes(fam.i2cBuses)) fail(`family ${id}: i2cBuses must be 1 or 2`);
  // The parser's channel pool (kMaxTotalChannels in src/config/Config.h) must match.
  {
    const header = readFileSync(join(root, 'src/config/Config.h'), 'utf8');
    const want = Number(new RegExp(id === 'esp8266'
      ? 'ESP8266\\)\\s*constexpr uint8_t kMaxTotalChannels = (\\d+)'
      : '#else\\s*constexpr uint8_t kMaxTotalChannels = (\\d+)').exec(header)?.[1]);
    if (fam.maxChannels !== want) fail(`family ${id}: maxChannels ${fam.maxChannels} differs from kMaxTotalChannels ${want} in src/config/Config.h`);
  }
  if (fam.configOffset != null && (fam.configOffset % 4096 !== 0)) fail(`family ${id}: configOffset must be sector aligned`);
  // The web app writes the config block where the catalog says, the firmware reads it where
  // main.cpp says. Both must be the same address.
  if (id === 'esp8266') {
    const main = readFileSync(join(root, 'src/main.cpp'), 'utf8');
    const fw = Number(/kConfigOffset8266\s*=\s*(0x[0-9A-Fa-f]+)/.exec(main)?.[1]);
    if (fam.configOffset !== fw) fail(`family esp8266: configOffset ${fam.configOffset} (0x${fam.configOffset?.toString(16)}) differs from kConfigOffset8266 0x${fw.toString(16)} in src/main.cpp`);
  }
}
for (const b of boards.boards) {
  if (boardIds.has(b.id)) fail(`board ${b.id}: duplicate`);
  boardIds.add(b.id);
  const fam = boards.families[b.family];
  if (!fam) { fail(`board ${b.id}: unknown family ${b.family}`); continue; }
  for (const pin of [...(b.exposed ?? []), b.i2c.sda, b.i2c.scl, ...(b.led != null ? [b.led] : [])]) {
    if (!fam.gpios.includes(pin)) fail(`board ${b.id}: pin ${pin} is not a gpio of ${b.family}`);
  }
  for (const pin of Object.keys(b.labels)) {
    if (!fam.gpios.includes(Number(pin))) fail(`board ${b.id}: label for unknown pin ${pin}`);
  }
  if (!['uart', 'native', 'both'].includes(b.usb)) fail(`board ${b.id}: usb must be uart|native|both`);

  // Default pins are what the web app suggests first, so each must work without a warning:
  // offered on the board, not a boot or console pin, not the I²C pair, ADC where analog.
  const offered = fam.gpios.filter((g) => !b.exclude.includes(g) && (!b.exposed?.length || b.exposed.includes(g)));
  const seenDefaults = new Set();
  for (const [bus, pins] of Object.entries(b.defaultPins ?? {})) {
    if (!['onewire', 'gpio', 'analog'].includes(bus)) fail(`board ${b.id}: defaultPins.${bus} is not a pin bus`);
    for (const pin of pins) {
      const at = `board ${b.id}: defaultPins.${bus} ${pin}`;
      if (!offered.includes(pin)) fail(`${at} is not offered on this board`);
      if (fam.strapping.includes(pin) || fam.serial.includes(pin)) fail(`${at} is a boot or console pin`);
      if (pin === b.i2c.sda || pin === b.i2c.scl) fail(`${at} is the I²C default`);
      if (bus === 'analog' ? !fam.adc.includes(pin) : fam.inputOnly.includes(pin)) fail(`${at} cannot do ${bus}`);
      if (seenDefaults.has(pin)) fail(`${at} is the default of another bus too`);
      seenDefaults.add(pin);
    }
  }
  if (!b.defaultPins) fail(`board ${b.id}: defaultPins missing`);
}

// --- drivers --------------------------------------------------------------------
const driverIds = new Set();
for (const d of drivers.drivers) {
  if (driverIds.has(d.id)) fail(`driver ${d.id}: duplicate`);
  driverIds.add(d.id);
  if (!['onewire', 'gpio', 'i2c', 'analog'].includes(d.bus)) fail(`driver ${d.id}: unknown bus ${d.bus}`);
  if (d.bus === 'i2c') {
    if (!d.addresses?.length || !d.addresses.includes(d.defaultAddress)) fail(`driver ${d.id}: i2c needs addresses incl. defaultAddress`);
  } else if (!d.pins?.length) {
    fail(`driver ${d.id}: needs pins`);
  }
  for (const ch of d.channels) {
    if (!drivers.quantities[ch.q]) fail(`driver ${d.id}: unknown quantity ${ch.q}`);
    if (!TYPE_RE.test(ch.defaultType)) fail(`driver ${d.id}: invalid default type ${ch.defaultType}`);
    for (const t of ch.typeOptions) if (t !== '*' && !TYPE_RE.test(t)) fail(`driver ${d.id}: invalid type option ${t}`);
  }
  for (const name of d.libs) if (!libByName.has(name)) fail(`driver ${d.id}: library ${name} missing in libraries.json`);
  for (const o of d.options) {
    if (!['enum', 'int', 'float'].includes(o.type)) fail(`driver ${d.id}: option ${o.key} has unknown type ${o.type}`);
    if (o.type === 'enum' && !o.values.includes(o.default)) fail(`driver ${d.id}: option ${o.key} default not in values`);
  }
}

if (errors.length) {
  console.error(`catalog invalid (${errors.length}):\n - ${errors.join('\n - ')}`);
  process.exit(1);
}

const outIdx = process.argv.indexOf('--out');
if (outIdx > 0) {
  const outDir = join(root, process.argv[outIdx + 1] ?? 'dist');
  mkdirSync(outDir, { recursive: true });
  const version = /^version\s*=\s*(\S+)/m.exec(ini)?.[1];
  const merged = {
    schema: 1,
    firmwareVersion: version,
    families: boards.families,
    boards: boards.boards,
    quantities: drivers.quantities,
    drivers: drivers.drivers,
    sleepModes: sleep.modes,
    libraries: libs.libraries,
    baseLibraries: libs.base,
    cores: libs.cores,
  };
  writeFileSync(join(outDir, 'catalog.json'), JSON.stringify(merged, null, 2) + '\n');
  console.log(`catalog ok → ${join(outDir, 'catalog.json')}`);
} else {
  console.log(`catalog ok: ${boards.boards.length} boards, ${drivers.drivers.length} drivers, ${libs.libraries.length} libraries`);
}
