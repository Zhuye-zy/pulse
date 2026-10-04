import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { spawnSync } from 'node:child_process';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const manifest = JSON.parse(await fs.readFile(path.join(here, 'manifest.json'), 'utf8'));
const referenceLimitations = JSON.parse(await fs.readFile(path.join(here, 'reference-limitations.json'), 'utf8'));
const cache = path.join(root, 'build/math_reference_cache');
const packageDir = path.join(cache, `katex-${manifest.version}`);
const archive = path.join(cache, `katex-${manifest.version}.tgz`);
const offline = process.argv.includes('--offline');
const snapshot = process.argv.includes('--update-snapshot');
const allowedArgs = new Set(['--offline', '--update-snapshot']);
if (process.argv.slice(2).some(arg => !allowedArgs.has(arg))) throw new Error('Usage: node tools/math_reference/run.mjs [--offline] [--update-snapshot]');
if (Number(process.versions.node.split('.')[0]) < manifest.nodeMinimumMajor) throw new Error(`Node ${manifest.nodeMinimumMajor}+ is required`);
await fs.mkdir(cache, { recursive: true });

const digest = data => crypto.createHash('sha256').update(data).digest('hex');
function verifyArchive(data) {
    const [algorithm, expected] = manifest.integrity.split('-');
    if (crypto.createHash(algorithm).update(data).digest('base64') !== expected) throw new Error('KaTeX archive integrity mismatch');
}
let tarball;
try { tarball = await fs.readFile(archive); }
catch (error) {
    if (error.code !== 'ENOENT' || offline) throw error;
    const response = await fetch(manifest.tarball, { signal: AbortSignal.timeout(90000) });
    if (!response.ok) throw new Error(`KaTeX download HTTP ${response.status}`);
    tarball = Buffer.from(await response.arrayBuffer());
    verifyArchive(tarball);
    await fs.writeFile(archive, tarball);
}
verifyArchive(tarball);

function tar(args) {
    const result = spawnSync('tar', args, { encoding: 'utf8', windowsHide: true, maxBuffer: 4 * 1024 * 1024 });
    if (result.error || result.status !== 0) throw new Error(`tar failed: ${result.error?.message ?? result.stderr}`);
    return result.stdout;
}
// Validate every archive path before extraction, even though its pinned hash is checked.
const entries = tar(['-tzf', archive]).trim().split(/\r?\n/);
if (entries.some(entry => !entry.startsWith('package/') || entry.split(/[\\/]/).includes('..')))
    throw new Error('Unexpected archive entry');
await fs.mkdir(packageDir, { recursive: true });
tar(['-xzf', archive, '-C', packageDir, '--strip-components=1']);
const katex = createRequire(import.meta.url)(path.join(packageDir, 'dist/katex.js'));
if (katex.version !== manifest.version) throw new Error(`KaTeX version mismatch: ${katex.version}`);
const stylesheet = await fs.readFile(path.join(packageDir, 'dist/katex.min.css'), 'utf8');
const fontFiles = new Set();
for (const match of stylesheet.matchAll(/url\(([^)]+)\)/g)) {
    const url = match[1].trim().replace(/^['"]|['"]$/g, '');
    if (!url.startsWith('fonts/') || url.split('/').includes('..')) throw new Error(`Nonlocal stylesheet dependency: ${url}`);
    await fs.access(path.join(packageDir, 'dist', url));
    fontFiles.add(url);
}
await fs.access(path.join(packageDir, 'LICENSE'));

const corpusBytes = await fs.readFile(path.join(here, manifest.corpus));
const lines = corpusBytes.toString('utf8').replace(/^\uFEFF/, '').trimEnd().split(/\r?\n/);
if (lines.shift() !== 'id\tdisplay\texpectation\tsource') throw new Error('Unexpected corpus header');
const ids = new Set();
const cases = lines.map(line => {
    const fields = line.split('\t');
    if (fields.length !== 4) throw new Error('Corpus rows must have four TSV fields');
    const [id, display, expectation, source] = fields;
    if (!/^[a-z0-9_]+$/.test(id) || ids.has(id) || !['0', '1'].includes(display) ||
        !['native_accept', 'native_fallback', 'invalid'].includes(expectation) || !source)
        throw new Error(`Invalid corpus row: ${id}`);
    ids.add(id);
    return { id, display: display === '1', expectation, source };
});
const escape = text => text.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const oneLine = value => String(value).replaceAll('\t', ' ').replaceAll('\r', ' ').replaceAll('\n', ' ');
const results = [];
for (const item of cases) {
    const limitation = referenceLimitations[item.id] ?? null;
    if (limitation && (limitation.source !== item.source || item.expectation !== 'native_accept'))
        throw new Error(`Reference limitation no longer matches its exact native contract: ${item.id}`);
    const result = { ...item, katexAccepted: false, mathmlTags: {}, mathmlConstructs: { brace: 0, phantom: 0 },
        fontVariants: {}, referenceLimitation: limitation, mathml: '', html: '', error: '' };
    try {
        const options = { ...manifest.options, displayMode: item.display, macros: {} };
        result.mathml = katex.renderToString(item.source, { ...options, output: 'mathml' });
        result.html = katex.renderToString(item.source, { ...options, macros: {}, output: 'htmlAndMathml' });
        result.katexAccepted = true;
        for (const match of result.mathml.matchAll(/<(m[a-z0-9]+)(?:\s|>)/g))
            result.mathmlTags[match[1]] = (result.mathmlTags[match[1]] ?? 0) + 1;
        result.mathmlConstructs.brace = [...result.mathml.matchAll(/<mo\b[^>]*\bstretchy="true"[^>]*>[⏞⏟]<\/mo>/g)].length;
        result.mathmlConstructs.phantom = result.mathmlTags.mphantom ?? 0;
        for (const match of result.mathml.matchAll(/\bmathvariant="([^"]+)"/g))
            result.fontVariants[match[1]] = (result.fontVariants[match[1]] ?? 0) + 1;
    } catch (error) { result.error = error.message; }
    result.expectedReferenceVerdict = limitation ? limitation.expectedAcceptance : item.expectation !== 'invalid';
    result.verdictMatches = result.katexAccepted === result.expectedReferenceVerdict;
    results.push(result);
}
const summary = {
    schema: 1, engine: 'KaTeX', version: katex.version, integrity: manifest.integrity,
    node: process.version, platform: process.platform, corpusSha256: digest(corpusBytes),
    options: manifest.options, total: results.length,
    accepted: results.filter(item => item.katexAccepted).length,
    rejected: results.filter(item => !item.katexAccepted).length,
    referenceVerdictMismatches: results.filter(item => !item.verdictMatches).map(item => item.id),
    nativeAcceptCases: cases.filter(item => item.expectation === 'native_accept').length,
    nativeFallbackCases: cases.filter(item => item.expectation === 'native_fallback').length,
    invalidCases: cases.filter(item => item.expectation === 'invalid').length,
    referenceLimitedCases: results.filter(item => item.referenceLimitation).map(item => item.id),
    formulaLocalMacroIsolation: {
        definitionCaseAccepted: results.find(item => item.id === 'macro_isolation_define')?.katexAccepted ?? null,
        followingUndefinedCaseRejected: results.find(item => item.id === 'macro_isolation_use_undefined')?.katexAccepted === false,
        freshMacrosPerCaseAndOutput: true
    },
    galleryAssets: { localStylesheet: true, verifiedLocalFontFiles: fontFiles.size, upstreamLicensePresent: true },
    note: 'Reference results only. Native acceptance and visual equivalence require separate checks.'
};
await fs.writeFile(path.join(cache, 'results.json'), JSON.stringify({ summary, results }, null, 2) + '\n');
await fs.writeFile(path.join(cache, 'summary.json'), JSON.stringify(summary, null, 2) + '\n');
const resultsTsv = [
    'id\tdisplay\texpectation\tsource\tkatex_accepted\tmathml_tags\terror\tmathml_constructs\tfont_variants\treference_limitation',
    ...results.map(item => [item.id, +item.display, item.expectation, item.source, +item.katexAccepted,
        Object.entries(item.mathmlTags).sort().map(([tag, count]) => `${tag}:${count}`).join(','), item.error || '-',
        Object.entries(item.mathmlConstructs).sort().map(([name, count]) => `${name}:${count}`).join(','),
        Object.entries(item.fontVariants).sort().map(([name, count]) => `${name}:${count}`).join(',') || '-',
        item.referenceLimitation?.reason ?? '-'].map(oneLine).join('\t'))
].join('\n') + '\n';
await fs.writeFile(path.join(cache, 'results.tsv'), resultsTsv);
if (snapshot) {
    if (summary.referenceVerdictMismatches.length) throw new Error('Refusing to snapshot reference verdict mismatches');
    await fs.writeFile(path.join(here, 'reference-results.tsv'), resultsTsv);
    await fs.writeFile(path.join(here, 'reference-summary.json'), JSON.stringify(summary, null, 2) + '\n');
}
await fs.mkdir(path.join(cache, 'mathml'), { recursive: true });
for (const item of results) if (item.katexAccepted)
    await fs.writeFile(path.join(cache, 'mathml', `${item.id}.mathml`), item.mathml + '\n');
const page = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pulse / KaTeX ${escape(katex.version)} reference</title>
<link rel="stylesheet" href="katex-${escape(katex.version)}/dist/katex.min.css">
<style>body{font:16px system-ui;margin:32px auto;max-width:1100px;padding:0 20px;color:#20232a;background:#fafafa}section{background:white;border:1px solid #d8dde4;border-radius:8px;padding:20px;margin:18px 0;overflow:auto}code{white-space:pre-wrap;overflow-wrap:anywhere;font-size:13px}h2{font-size:18px}.meta{color:#58616e;font-size:13px}.error{color:#a51616}.render{padding:16px 0;min-height:40px}button{padding:8px 12px}body.dark{background:#121820;color:#e7edf4}body.dark section{background:#1b2430;border-color:#39485a}body.dark .meta{color:#a5b2c3}</style>
<h1>KaTeX ${escape(katex.version)} development reference</h1><p>This is an independent reference, not the Pulse renderer. ${summary.accepted} accepted, ${summary.rejected} rejected. Original formulas; no product dependency. Corpus SHA-256: <code>${summary.corpusSha256}</code>.</p>
<button onclick="document.body.classList.toggle('dark')">Toggle reference theme</button>
${results.map(item => `<section id="${item.id}"><h2>${escape(item.id)}</h2><p class="meta">${escape(item.expectation)} · ${item.display ? 'display' : 'inline'} · KaTeX ${item.katexAccepted ? 'accepted' : 'rejected'}</p><code>${escape(item.source)}</code><div class="render">${item.katexAccepted ? item.html : `<p class="error">${escape(item.error)}</p>`}</div><p class="meta">${escape(JSON.stringify(item.mathmlTags))}</p></section>`).join('\n')}</html>`;
await fs.writeFile(path.join(cache, 'reference.html'), page);
console.log(JSON.stringify(summary, null, 2));
console.log(`Artifacts: ${cache}`);
if (summary.referenceVerdictMismatches.length) process.exitCode = 1;
