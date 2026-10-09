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
// "owner/Name@1.2.3" from the registry, or "https://….git#1.2.3" for a release tag on GitHub.
// "symlink://../hydronode-library" is a development branch building against the sibling checkout
// of our own library: it stands for the GitHub entry with the version that checkout declares.
const iniDeps = new Map(depLines.map((l) => {
  if (l.startsWith('symlink://')) {
    const dir = join(root, l.slice('symlink://'.length));
    const props = readFileSync(join(dir, 'library.properties'), 'utf8');
    if (!/^name=HydroNode-Library$/m.test(props)) fail(`platformio.ini: ${l} is not HydroNode-Library`);
    return ['https://github.com/TexhFexLabs/HydroNode-Library.git', /^version=(\S+)/m.exec(props)?.[1]];
  }
  const at = l.startsWith('https://') ? l.lastIndexOf('#') : l.lastIndexOf('@');
  return [l.slice(0, at), l.slice(at + 1)];
}));
// [env:esp8266] uses the same list as [esp].
const esp8266Block = ini.split(/^\[env:esp8266\]\s*$/m)[1]?.split(/^\[/m)[0] ?? '';
if (esp8266Block && !/^lib_deps\s*=\s*\$\{esp\.lib_deps\}\s*$/m.test(esp8266Block)) {
  fail('[env:esp8266]: lib_deps must be ${esp.lib_deps}');
}
for (const lib of libs.libraries) {
  if (iniDeps.get(lib.pio) !== lib.version) fail(`platformio.ini: ${lib.pio}@${lib.version} expected, found ${iniDeps.get(lib.pio) ?? 'nothing'}`);
}
for (const [pio] of iniDeps) {
  if (!libs.libraries.some((l) => l.pio === pio)) fail(`platformio.ini: ${pio} is not listed in catalog/libraries.json`);
}

// --- firmware versions ----------------------------------------------------------
// "minFirmware" on a driver or sleep mode: the first firmware that runs it. Fleet sends a config
// that uses it only to a device on that version or newer, or together with the firmware update.
// Without it, a part works on every firmware that takes config over the air (0.5.0).
const CONFIG_OTA_MIN = '0.5.0';
const firmwareVersion = /^version\s*=\s*(\S+)/m.exec(ini)?.[1];
const semver = (v) => /^(\d+)\.(\d+)\.(\d+)$/.exec(v ?? '')?.slice(1).map(Number) ?? null;
const compare = (a, b) => {
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return Math.sign(a[i] - b[i]);
  return 0;
};
function checkMinFirmware(owner, value) {
  if (value === undefined) return;
  const v = semver(value);
  if (!v) return fail(`${owner}: minFirmware must be X.Y.Z, got ${value}`);
  if (compare(v, semver(CONFIG_OTA_MIN)) < 0) fail(`${owner}: minFirmware below ${CONFIG_OTA_MIN} means nothing, leave it out`);
  if (semver(firmwareVersion) && compare(v, semver(firmwareVersion)) > 0) {
    fail(`${owner}: minFirmware ${value} is newer than this firmware (${firmwareVersion})`);
  }
}

// --- sleep modes ---------------------------------------------------------------
const modeIds = new Set(sleep.modes.map((m) => m.id));
for (const m of sleep.modes) checkMinFirmware(`sleep mode ${m.id}`, m.minFirmware);

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
  // The web app adds these up to the fastest interval a configuration can keep.
  for (const key of ['bootMs', 'wifiMs', 'wifiFastMs', 'connectMs', 'valueMs']) {
    if (!(fam.timing?.[key] > 0)) fail(`family ${id}: timing.${key} missing`);
  }
  // Currents for the battery estimate in the web app: CPU awake, WiFi on, modem sleep, light sleep.
  for (const key of ['cpuMa', 'wifiMa', 'modemMa', 'lightUa']) {
    if (!(fam.current?.[key] > 0)) fail(`family ${id}: current.${key} missing`);
  }
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
  // src/config/ConfigStore.cpp says. Both must be the same address.
  if (id === 'esp8266') {
    const main = readFileSync(join(root, 'src/config/ConfigStore.cpp'), 'utf8');
    const fw = Number(/kConfigOffset8266\s*=\s*(0x[0-9A-Fa-f]+)/.exec(main)?.[1]);
    if (fam.configOffset !== fw) fail(`family esp8266: configOffset ${fam.configOffset} (0x${fam.configOffset?.toString(16)}) differs from kConfigOffset8266 0x${fw.toString(16)} in src/config/ConfigStore.cpp`);
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
  // Whole board in deep sleep (chip, regulator, USB chip, power LED), for the battery estimate.
  if (!(b.sleepUa > 0)) fail(`board ${b.id}: sleepUa missing`);
}

// --- sleep rules -----------------------------------------------------------------
// How a part behaves per power mode. Rules apply when every condition matches: `modes`, optional
// `families`, and `when` (option values; `$pin`/`$pin2`/`$powerPin` whether that pin is wired;
// `$channel` whether a value with that quantity is sent). `blocked` rules out the mode (with a
// `fix`), `minFirmware` names the first firmware that runs it, `note` explains a trade-off,
// `waitMs`/`sleepUa` replace the part's numbers in that mode. Firmware, backend and web app read
// the same rules.
const SLEEP_KEYS = ['modes', 'families', 'when', 'blocked', 'fix', 'minFirmware', 'note', 'waitMs', 'sleepUa'];
function checkSleepRules(d) {
  if (!Array.isArray(d.sleep)) return fail(`driver ${d.id}: sleep rules missing (use [] for none)`);
  d.sleep.forEach((rule, i) => {
    const at = `driver ${d.id}: sleep[${i}]`;
    for (const key of Object.keys(rule)) if (!SLEEP_KEYS.includes(key)) fail(`${at}: unknown key ${key}`);
    if (!rule.modes?.length || rule.modes.some((m) => !modeIds.has(m))) fail(`${at}: modes must name sleep modes`);
    for (const f of rule.families ?? []) if (!boards.families[f]) fail(`${at}: unknown family ${f}`);
    for (const [key, value] of Object.entries(rule.when ?? {})) {
      if (key === '$channel') {
        if (!d.channels.some((c) => c.q === value)) fail(`${at}: $channel ${value} is not a value of this part`);
      } else if (key.startsWith('$')) {
        if (!['$pin', '$pin2', '$powerPin'].includes(key) || typeof value !== 'boolean') fail(`${at}: unknown condition ${key}`);
      } else {
        const option = d.options.find((o) => o.key === key);
        if (!option) fail(`${at}: when.${key} is not an option`);
        else if (option.type === 'enum' && !option.values.includes(value)) fail(`${at}: when.${key} ${value} is not a value`);
      }
    }
    if (rule.blocked && !rule.fix) fail(`${at}: a blocked mode says how to get around it (fix)`);
    if (rule.blocked && (rule.minFirmware || rule.waitMs != null || rule.sleepUa != null)) fail(`${at}: a blocked rule only explains`);
    if (!rule.blocked && !rule.minFirmware && !rule.note && rule.waitMs == null && rule.sleepUa == null) fail(`${at}: rule does nothing`);
    if (rule.blocked && /\.$/.test(rule.blocked)) fail(`${at}: blocked ends a sentence after the part's name, no full stop`);
    checkMinFirmware(at, rule.minFirmware);
  });
}

// --- drivers --------------------------------------------------------------------
// Ids the firmware knows: createDriver() in src/drivers/Drivers.cpp, isActuator() in
// src/actuators/Actuators.cpp. A catalog entry without firmware support would flash a device that
// reports "ERR SENSOR <id> unknown".
const firmwareIds = new Set([
  ...readFileSync(join(root, 'src/drivers/Drivers.cpp'), 'utf8').matchAll(/strcmp\(id, "([a-z0-9]+)"\) == 0/g),
  ...readFileSync(join(root, 'src/actuators/Actuators.cpp'), 'utf8').matchAll(/strcmp\(driver, "([a-z0-9]+)"\) == 0/g),
].map((m) => m[1]));
const driverIds = new Set();
const OPTION_TYPES = ['enum', 'int', 'float', 'bool', 'text'];
for (const d of drivers.drivers) {
  if (driverIds.has(d.id)) fail(`driver ${d.id}: duplicate`);
  driverIds.add(d.id);
  if (!firmwareIds.has(d.id)) fail(`driver ${d.id}: the firmware does not know this id`);
  if (!['onewire', 'gpio', 'i2c', 'analog', 'none'].includes(d.bus)) fail(`driver ${d.id}: unknown bus ${d.bus}`);
  // gauge (0.8.0): measures the battery the thresholds watch; the builder offers it in the power step.
  if (!['sensor', 'output', 'input', 'gauge', undefined].includes(d.kind)) fail(`driver ${d.id}: unknown kind ${d.kind}`);
  if (d.kind === 'gauge' && (d.bus !== 'i2c' || !d.channels.some((c) => c.defaultType === 'BATTERY_VOLTAGE'))) {
    fail(`driver ${d.id}: a gauge sits on I²C and measures BATTERY_VOLTAGE`);
  }
  if (d.bus === 'i2c') {
    if (!d.addresses?.length || !d.addresses.includes(d.defaultAddress)) fail(`driver ${d.id}: i2c needs addresses incl. defaultAddress`);
  } else if (d.bus !== 'none' && !d.pins?.length) {
    fail(`driver ${d.id}: needs pins`);
  }
  // The config carries at most two pins per device: "pin" and "pin2".
  (d.pins ?? []).forEach((pin, i) => {
    if (pin.key !== ['pin', 'pin2'][i]) fail(`driver ${d.id}: pin ${i + 1} must have key ${['pin', 'pin2'][i] ?? '(none, at most two)'}`);
  });
  // Timing per due round, the same numbers the firmware waits and the web app adds up.
  for (const key of ['bootMs', 'powerUpMs', 'waitMs', 'readMs', 'sleepUa', 'activeMa']) {
    if (!(d[key] >= 0)) fail(`driver ${d.id}: ${key} missing`);
  }
  for (const key of ['warmupMs', 'sleepSafe', 'continuous', 'requiresAwake']) {
    if (key in d) fail(`driver ${d.id}: ${key} is replaced by the sleep rules`);
  }
  if ((d.pins ?? []).some((pin, i) => pin.optional && i === 0 && d.bus !== 'i2c')) {
    fail(`driver ${d.id}: only a second pin, or the pin of an I²C part, can be optional`);
  }
  checkSleepRules(d);
  if (d.kind === 'output' ? d.channels.length !== 0 : d.channels.length === 0) {
    fail(`driver ${d.id}: ${d.kind === 'output' ? 'outputs send nothing' : 'needs at least one channel'}`);
  }
  const qs = new Set();
  for (const ch of d.channels) {
    if (!drivers.quantities[ch.q]) fail(`driver ${d.id}: unknown quantity ${ch.q}`);
    if (qs.has(ch.q)) fail(`driver ${d.id}: quantity ${ch.q} twice`);
    qs.add(ch.q);
    if (!TYPE_RE.test(ch.defaultType)) fail(`driver ${d.id}: invalid default type ${ch.defaultType}`);
    for (const t of ch.typeOptions) if (t !== '*' && !TYPE_RE.test(t)) fail(`driver ${d.id}: invalid type option ${t}`);
    // A value newer than its part (an INA's battery level): only sent by that firmware or newer.
    checkMinFirmware(`driver ${d.id}: channel ${ch.q}`, ch.minFirmware);
  }
  for (const name of d.libs) if (!libByName.has(name)) fail(`driver ${d.id}: library ${name} missing in libraries.json`);
  // Sampling every second needs a running CPU; such a sensor can never be sleep safe.
  checkMinFirmware(`driver ${d.id}`, d.minFirmware);
  if (d.options.length > 6) fail(`driver ${d.id}: at most 6 options (kMaxOptions in src/config/Config.h)`);
  for (const o of d.options) {
    if (!OPTION_TYPES.includes(o.type)) fail(`driver ${d.id}: option ${o.key} has unknown type ${o.type}`);
    if (o.key.length > 15) fail(`driver ${d.id}: option key ${o.key} longer than 15 characters`);
    if (o.type === 'enum' && !o.values.includes(o.default)) fail(`driver ${d.id}: option ${o.key} default not in values`);
    if (o.type === 'enum' && o.values.some((v) => v.length > 15)) fail(`driver ${d.id}: option ${o.key} value longer than 15 characters`);
    if (o.type === 'bool' && typeof o.default !== 'boolean') fail(`driver ${d.id}: option ${o.key} default must be true or false`);
    if (o.type === 'text') {
      if (!o.pattern) fail(`driver ${d.id}: text option ${o.key} needs a pattern`);
      else if (!new RegExp(o.pattern).test(o.default)) fail(`driver ${d.id}: option ${o.key} default does not match its pattern`);
    }
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
  const merged = {
    schema: 1,
    firmwareVersion,
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
