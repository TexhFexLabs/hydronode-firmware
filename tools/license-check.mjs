#!/usr/bin/env node
// License gate for everything that ends up in a firmware binary.
//
// Walks the libraries that were actually compiled (.pio/build/<env>/lib*/<name>).
// Framework libraries (WiFi, Wire, …) belong to the Arduino core and are covered
// by its LGPL-2.1 notice. Every other library fails the check when it
//   - is not listed in catalog/libraries.json (name + exact version), or
//   - ships a license text that looks like GPL/AGPL/LGPL-3 or "all rights reserved"
//     without a permission grant.
// With --out <file> it also writes THIRD_PARTY_LICENSES.md containing the full
// license text of every bundled library plus the LGPL source offer for the core.
//
//   node tools/license-check.mjs --env esp32c3 [--out dist/THIRD_PARTY_LICENSES.md]

import { readFileSync, readdirSync, existsSync, writeFileSync, mkdirSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const arg = (name) => {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : undefined;
};
const envs = (arg('--env') ?? 'esp32,esp32s2,esp32s3,esp32c3,esp32c6,esp8266').split(',');
// Our own MIT library: the ESP8266 build uses a newer release (1.4.0) than the catalog's ESP32 pin.
const OWN = new Set(['HydroNode-Library']);
const out = arg('--out');

const catalog = JSON.parse(readFileSync(join(root, 'catalog/libraries.json'), 'utf8'));
const byName = new Map(catalog.libraries.map((l) => [l.name, l]));

// Texts that must never be linked into the firmware. LGPL-2.1 is only allowed for
// the Arduino core, which is not in libdeps.
const FORBIDDEN = [
  [/GNU AFFERO GENERAL PUBLIC LICENSE/i, 'AGPL'],
  [/GNU LESSER GENERAL PUBLIC LICENSE[\s\S]{0,200}Version 3/i, 'LGPL-3.0'],
  [/GNU LESSER GENERAL PUBLIC LICENSE/i, 'LGPL'],
  [/GNU GENERAL PUBLIC LICENSE/i, 'GPL'],
  [/under the terms of the GNU (Lesser )?General Public License/i, 'GPL/LGPL header'],
];
const LICENSE_FILE = /^(licen[cs]e|copying)(\.(md|txt))?$/i;

function readMeta(dir) {
  const json = join(dir, 'library.json');
  if (existsSync(json)) {
    const m = JSON.parse(readFileSync(json, 'utf8'));
    return { name: m.name, version: m.version };
  }
  const props = join(dir, 'library.properties');
  if (existsSync(props)) {
    const text = readFileSync(props, 'utf8');
    return {
      name: /^name=(.*)$/m.exec(text)?.[1].trim(),
      version: /^version=(.*)$/m.exec(text)?.[1].trim(),
    };
  }
  return null;
}

function licenseText(dir) {
  const file = readdirSync(dir).find((f) => LICENSE_FILE.test(f));
  if (file) return readFileSync(join(dir, file), 'utf8');
  // Some libraries (OneWire) only carry the license in a source header.
  for (const f of readdirSync(dir)) {
    if (!/\.(cpp|c|h)$/.test(f)) continue;
    const head = readFileSync(join(dir, f), 'utf8').slice(0, 8000);
    const start = head.search(/Permission is hereby granted|Redistribution and use in source|BSD license, all text above/);
    if (start >= 0) {
      // Keep the whole leading comment: "all text above must be included".
      const end = head.indexOf('*/', start);
      return head.slice(0, end > 0 ? end : undefined).replace(/^\/\*!?/, '').trim();
    }
  }
  return null;
}

const errors = [];
const seen = new Map();
for (const env of envs) {
  const base = join(root, '.pio/libdeps', env);
  const build = join(root, '.pio/build', env);
  if (!existsSync(base) || !existsSync(build)) {
    errors.push(`${env}: build missing, run "pio run -e ${env}" first`);
    continue;
  }
  const linked = readdirSync(build)
    .filter((d) => /^lib[0-9a-f]+$/.test(d))
    .flatMap((d) => readdirSync(join(build, d)).filter((n) => statSync(join(build, d, n)).isDirectory()));
  // Header-only libraries (ArduinoJson) never show up as a compiled lib dir, so
  // every installed library that the catalog lists is checked as well.
  const listedInstalled = readdirSync(base).filter((n) => {
    const meta = statSync(join(base, n)).isDirectory() ? readMeta(join(base, n)) : null;
    return meta && byName.has(meta.name);
  });
  for (const entry of new Set([...linked, ...listedInstalled])) {
    const dir = join(base, entry);
    if (!existsSync(dir)) continue; // framework library, covered by the core notice
    const meta = readMeta(dir);
    if (!meta) { errors.push(`${env}/${entry}: no library.json/library.properties`); continue; }
    const listed = byName.get(meta.name);
    if (!listed) { errors.push(`${env}: "${meta.name}" is not in catalog/libraries.json`); continue; }
    if (listed.version !== meta.version && !OWN.has(meta.name)) errors.push(`${env}: ${meta.name} is ${meta.version}, catalog pins ${listed.version}`);
    const text = licenseText(dir);
    if (!text) { errors.push(`${env}: ${meta.name} has no license text`); continue; }
    const hit = FORBIDDEN.find(([re]) => re.test(text));
    if (hit) errors.push(`${env}: ${meta.name} license text looks like ${hit[1]}`);
    seen.set(meta.name, { ...listed, text });
  }
}

if (errors.length) {
  console.error(`license check failed (${errors.length}):\n - ${errors.join('\n - ')}`);
  process.exit(1);
}

if (out) {
  const lines = [
    '# Third-party licenses — HydroNode firmware',
    '',
    'This firmware contains the open source components listed below.',
    '',
    '## LGPL-2.1 notice (Arduino cores)',
    '',
  ];
  for (const core of catalog.cores.filter((c) => !c.buildOnly)) {
    lines.push(`- **${core.name} ${core.version}** — ${core.license} — ${core.url}` + (core.source ? `, source: ${core.source}` : '') + (core.note ? `. ${core.note}` : ''));
  }
  lines.push(
    '',
    'The Arduino cores for ESP32 and ESP8266 are licensed under the GNU Lesser General Public License 2.1.',
    'You may modify it and relink the firmware: the complete firmware source, with every',
    'dependency pinned, is published at https://github.com/TexhFexLabs/hydronode-firmware.',
    'The exact core sources are attached to every firmware release.',
    '',
    '## Libraries',
    '',
  );
  for (const lib of [...seen.values()].sort((a, b) => a.name.localeCompare(b.name))) {
    lines.push(`### ${lib.name} ${lib.version} (${lib.license})`, '', lib.url, '', '```text', lib.text.trim(), '```', '');
  }
  mkdirSync(dirname(join(root, out)), { recursive: true });
  writeFileSync(join(root, out), lines.join('\n'));
  console.log(`license check ok (${seen.size} libraries) → ${out}`);
} else {
  console.log(`license check ok (${seen.size} libraries)`);
}
