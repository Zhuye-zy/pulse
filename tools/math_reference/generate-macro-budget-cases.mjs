import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// Deterministic authored boundary fixtures; no native parser or renderer is used.
const here = path.dirname(fileURLToPath(import.meta.url));
const file = path.join(here, 'corpus.tsv');
const name = index => `\\pulse${String.fromCharCode(65 + Math.floor(index / 26))}${String.fromCharCode(65 + index % 26)}`;
const definitions = count => Array.from({ length: count }, (_, i) => `\\newcommand{${name(i)}}{${i + 1}}`).join('') + `${name(count - 1)}+1=${count + 1}`;
const calls = count => '\\newcommand{\\pulseEmpty}{}' + '\\pulseEmpty'.repeat(count) + ' x+1=2';
const depth = count => Array.from({ length: count }, (_, i) => `\\newcommand{${name(i)}}{${i + 1 < count ? name(i + 1) : 'x'}}`).join('') + `${name(0)}^2+1=x^2+1`;
const rows = [
    ['macro_definitions_at_limit', '1', 'native_accept', definitions(64)],
    ['macro_definitions_over_limit', '1', 'native_fallback', definitions(65)],
    ['macro_calls_at_limit', '0', 'native_accept', calls(256)],
    ['macro_calls_over_limit', '0', 'native_fallback', calls(257)],
    ['macro_depth_at_limit', '0', 'native_accept', depth(32)],
    ['macro_depth_over_limit', '0', 'native_fallback', depth(33)],
    ['macro_expanded_size_over_limit', '0', 'native_fallback', '\\newcommand{\\pulseDup}[1]{#1#1}y+\\pulseDup{' + '1'.repeat(2100) + '}+z'],
    ['formula_raw_size_over_limit', '0', 'native_fallback', 'x=' + '1+'.repeat(2050) + '0']
];
const generatedIds = new Set(rows.map(row => row[0]));
const previous = (await fs.readFile(file, 'utf8')).trimEnd().split(/\r?\n/);
const retained = previous.filter(line => !generatedIds.has(line.split('\t')[0]));
await fs.writeFile(file, [...retained, ...rows.map(row => row.join('\t'))].join('\n') + '\n');
console.log(`Updated ${rows.length} deterministic macro budget cases in ${file}`);
