import fs from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const snapshot = process.argv.includes('--update-snapshot');
const arguments_ = process.argv.slice(2).filter(arg => arg !== '--update-snapshot');
if (arguments_.length > 1 || arguments_.some(arg => arg.startsWith('--')))
    throw new Error('Usage: node tools/math_reference/compare.mjs [native-results.tsv] [--update-snapshot]');
const nativePath = path.resolve(arguments_[0] ?? path.join(root, 'build/math-native-compat.tsv'));
const output = path.join(root, 'build/math_reference_cache');
const manifest = JSON.parse(await fs.readFile(path.join(here, 'manifest.json'), 'utf8'));
const baseline = JSON.parse(await fs.readFile(path.join(here, 'reference-summary.json'), 'utf8'));
const metricsExceptions = JSON.parse(await fs.readFile(path.join(here, 'metrics-exceptions.json'), 'utf8'));
const referenceLimitations = JSON.parse(await fs.readFile(path.join(here, 'reference-limitations.json'), 'utf8'));
const corpusBytes = await fs.readFile(path.join(here, manifest.corpus));
const corpusHash = crypto.createHash('sha256').update(corpusBytes).digest('hex');
if (baseline.corpusSha256 !== corpusHash || baseline.version !== manifest.version || baseline.integrity !== manifest.integrity)
    throw new Error('Reference snapshot is stale; rerun the pinned reference and review --update-snapshot');

function parseTsv(text, required, label) {
    const lines = text.replace(/^\uFEFF/, '').trimEnd().split(/\r?\n/);
    const headers = lines.shift().split('\t');
    if (new Set(headers).size !== headers.length || required.some(key => !headers.includes(key)))
        throw new Error(`${label}: missing or duplicate headers`);
    const rows = new Map();
    for (const line of lines) {
        const values = line.split('\t');
        // A final empty error column may be removed by trimEnd on the last row.
        if (values.length > headers.length) throw new Error(`${label}: too many columns`);
        const row = Object.fromEntries(headers.map((key, index) => [key, values[index] ?? '']));
        if (!row.id || rows.has(row.id)) throw new Error(`${label}: missing or duplicate id ${row.id}`);
        rows.set(row.id, row);
    }
    return rows;
}
function counts(text) {
    const result = {};
    for (const part of text.split(',').filter(Boolean)) {
        const match = /^([a-z0-9_]+):(\d+)$/.exec(part);
        if (!match || Object.hasOwn(result, match[1])) throw new Error(`Invalid structure summary: ${text}`);
        result[match[1]] = Number(match[2]);
    }
    return result;
}
const corpus = parseTsv(corpusBytes.toString('utf8'), ['id', 'display', 'expectation', 'source'], 'corpus');
const referenceBytes = await fs.readFile(path.join(here, 'reference-results.tsv'));
const nativeBytes = await fs.readFile(nativePath);
const reference = parseTsv(referenceBytes.toString('utf8'),
    ['id', 'display', 'expectation', 'source', 'katex_accepted', 'mathml_tags', 'mathml_constructs', 'font_variants', 'reference_limitation'], 'reference');
const native = parseTsv(nativeBytes.toString('utf8'), ['id', 'expectation', 'native_accepted', 'width', 'height', 'baseline', 'native_structures'], 'native');
const rows = [];
for (const [id, item] of corpus) {
    const ref = reference.get(id), own = native.get(id);
    const limitation = referenceLimitations[id] ?? null;
    const problems = [];
    let classification = 'unverified';
    const structureDifferences = [];
    if (!ref || !own) problems.push(!ref ? 'missing_reference' : 'missing_native');
    if (ref && (ref.source !== item.source || ref.display !== item.display || ref.expectation !== item.expectation))
        problems.push('reference_corpus_mismatch');
    if (own && own.expectation !== item.expectation) problems.push('native_expectation_mismatch');
    if (limitation && (limitation.source !== item.source || item.expectation !== 'native_accept' || ref?.reference_limitation !== limitation.reason))
        problems.push('reference_limitation_contract_mismatch');
    if (ref && !['0', '1'].includes(ref.katex_accepted)) problems.push('invalid_reference_verdict');
    if (own && !['0', '1'].includes(own.native_accepted)) problems.push('invalid_native_verdict');
    if (!problems.length) {
        const nativeAccepted = own.native_accepted === '1', referenceAccepted = ref.katex_accepted === '1';
        if (item.expectation === 'native_accept') {
            if (limitation) {
                classification = nativeAccepted && referenceAccepted === limitation.expectedAcceptance ? 'native_accept_reference_limited' : 'reference_limitation_contract_changed';
                if (classification !== 'native_accept_reference_limited') problems.push('expected_native_accept_documented_reference_limitation');
            } else {
                classification = nativeAccepted && referenceAccepted ? 'both_accept' : 'acceptance_regression';
                if (classification !== 'both_accept') problems.push('expected_both_accept');
            }
        } else if (item.expectation === 'native_fallback') {
            classification = !nativeAccepted && referenceAccepted ? 'documented_native_fallback' : 'fallback_contract_changed';
            if (classification !== 'documented_native_fallback') problems.push('expected_reference_only');
        } else {
            classification = !nativeAccepted && !referenceAccepted ? 'both_reject' : 'invalid_input_accepted';
            if (classification !== 'both_reject') problems.push('expected_both_reject');
        }
        if (nativeAccepted && referenceAccepted) {
            if (!own.native_structures) problems.push('missing_native_structure_evidence');
            else {
                const a = counts(own.native_structures), b = counts(ref.mathml_tags);
                const constructs = counts(ref.mathml_constructs);
                const comparable = { fraction: b.mfrac ?? 0, radical: (b.msqrt ?? 0) + (b.mroot ?? 0), environment: b.mtable ?? 0,
                    phantom: constructs.phantom ?? 0, brace: constructs.brace ?? 0 };
                for (const [name, referenceCount] of Object.entries(comparable)) {
                    if (!Object.hasOwn(a, name)) problems.push(`missing_native_${name}_count`);
                    else if (a[name] !== referenceCount) structureDifferences.push({ structure: name, native: a[name], reference: referenceCount });
                }
            }
            if (['width', 'height', 'baseline'].every(key => key in own)) {
                const width = Number(own.width), height = Number(own.height), baseline = Number(own.baseline);
                const exception = metricsExceptions[id];
                if (exception && (exception.source !== item.source || item.expectation !== 'native_accept'))
                    problems.push('metrics_exception_source_mismatch');
                const zeroWidthAllowed = exception?.allowZeroWidth && exception.source === item.source && item.expectation === 'native_accept';
                if (![width, height, baseline].every(Number.isFinite) || (zeroWidthAllowed ? width < 0 : width <= 0) || height <= 0 || baseline < 0 || baseline > height)
                    problems.push('invalid_native_metrics');
            }
        }
    }
    rows.push({ id, expectation: item.expectation, nativeAccepted: own?.native_accepted ?? null,
        referenceAccepted: ref?.katex_accepted ?? null, classification, problems, structureDifferences,
        nativeStructures: own?.native_structures ?? '', referenceMathmlTags: ref?.mathml_tags ?? '',
        referenceConstructs: ref?.mathml_constructs ?? '', metricsException: metricsExceptions[id] ?? null,
        nativeMetrics: own ? { width: Number(own.width), height: Number(own.height), baseline: Number(own.baseline) } : null,
        referenceLimitation: limitation, referenceFontVariantsInformational: ref?.font_variants ?? '' });
}
const extraNativeIds = [...native.keys()].filter(id => !corpus.has(id));
const extraReferenceIds = [...reference.keys()].filter(id => !corpus.has(id));
const summary = {
    schema: 1, referenceEngine: baseline.engine, referenceVersion: baseline.version, corpusSha256: corpusHash,
    nativeInput: path.relative(root, nativePath).replaceAll('\\', '/'), total: rows.length,
    nativeInputSha256: crypto.createHash('sha256').update(nativeBytes).digest('hex'),
    referenceInputSha256: crypto.createHash('sha256').update(referenceBytes).digest('hex'),
    bothAccept: rows.filter(row => row.classification === 'both_accept').length,
    nativeAcceptedReferenceLimited: rows.filter(row => row.classification === 'native_accept_reference_limited').length,
    documentedNativeFallback: rows.filter(row => row.classification === 'documented_native_fallback').length,
    bothReject: rows.filter(row => row.classification === 'both_reject').length,
    problemCases: rows.filter(row => row.problems.length).map(row => row.id),
    structureReviewCases: rows.filter(row => row.structureDifferences.length).map(row => row.id),
    extraNativeIds, extraReferenceIds,
    comparedStructures: ['fraction / mfrac', 'radical / msqrt + mroot', 'environment / mtable', 'phantom / mphantom', 'brace / stretchy brace mo'],
    note: 'Matching counts do not prove AST semantics, glyph fidelity or visual equivalence. Script, accent and operator-limit MathML overlaps are intentionally not equated.'
};
await fs.mkdir(output, { recursive: true });
await fs.writeFile(path.join(output, 'comparison.json'), JSON.stringify({ summary, rows }, null, 2) + '\n');
if (snapshot) await fs.writeFile(path.join(root, 'docs/markdown-math-compatibility.json'), JSON.stringify({ summary, rows }, null, 2) + '\n');
await fs.writeFile(path.join(output, 'comparison.tsv'), ['id\texpectation\tnative_accepted\tkatex_accepted\tclassification\tproblems\tstructure_review',
    ...rows.map(row => [row.id, row.expectation, row.nativeAccepted ?? '', row.referenceAccepted ?? '', row.classification,
        row.problems.join(','), row.structureDifferences.map(diff => `${diff.structure}:native=${diff.native}/reference=${diff.reference}`).join(',')].join('\t'))].join('\n') + '\n');
await fs.writeFile(path.join(output, 'comparison.md'), `# Native / KaTeX ${baseline.version} comparison\n\n` +
    `Corpus: ${corpusHash}\n\nNative report: ${summary.nativeInput}\n\n` +
    `Both accept: ${summary.bothAccept}; native accepted / documented reference limitation: ${summary.nativeAcceptedReferenceLimited}; documented native fallback: ${summary.documentedNativeFallback}; both reject: ${summary.bothReject}.\n\n` +
    `Problem cases: ${summary.problemCases.join(', ') || 'none'}. Structure review cases: ${summary.structureReviewCases.join(', ') || 'none'}.\n\n` +
    `Compared counts: fractions, radicals, environments, phantoms and braces. ${summary.note}\n\n` +
    '| Case | Classification | Problems | Structural review |\n| --- | --- | --- | --- |\n' + rows.map(row =>
        `| ${row.id} | ${row.classification} | ${row.problems.join(', ')} | ${row.structureDifferences.map(diff => `${diff.structure}: native ${diff.native}, reference ${diff.reference}`).join('; ')} |`).join('\n') + '\n');
console.log(JSON.stringify(summary, null, 2));
console.log(`Comparison artifacts: ${output}`);
// Count differences need review, not an automatic assertion of equivalent layout.
if (summary.problemCases.length || summary.structureReviewCases.length || extraNativeIds.length || extraReferenceIds.length) process.exitCode = 1;
