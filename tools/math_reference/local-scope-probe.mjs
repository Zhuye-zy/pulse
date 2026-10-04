import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const manifest = JSON.parse(await fs.readFile(path.join(here, 'manifest.json'), 'utf8'));
const katex = createRequire(import.meta.url)(path.join(root, `build/math_reference_cache/katex-${manifest.version}/dist/katex.js`));
if (katex.version !== manifest.version) throw new Error('Reference version mismatch');
const probes = [
    { id: 'matrix_cell_renewal', source: String.raw`\newcommand{\f}{a}\begin{matrix}\renewcommand{\f}{b}\f&1\\0&\f\end{matrix}+\f` },
    { id: 'substack_row_renewal', source: String.raw`\newcommand{\f}{a}\substack{\renewcommand{\f}{b}\f\\\f}+\f` },
    { id: 'substack_row_new_definition', source: String.raw`\substack{\newcommand{\f}{b}\f\\\f}` }
];
const results = probes.map(probe => {
    try {
        const mathml = katex.renderToString(probe.source, { ...manifest.options, macros: {}, output: 'mathml', displayMode: true });
        return { ...probe, accepted: true, variables: [...mathml.matchAll(/<mi\b[^>]*>([^<]*)<\/mi>/g)].map(match => match[1]), mathml };
    } catch (error) { return { ...probe, accepted: false, error: error.message }; }
});
const report = { reference: 'KaTeX', version: katex.version, freshMacrosPerProbe: true, results };
await fs.writeFile(path.join(here, 'local-scope-results.json'), JSON.stringify(report, null, 2) + '\n');
for (const item of results) console.log(JSON.stringify({ id: item.id, accepted: item.accepted, variables: item.variables, error: item.error }));
