# Development mathematical reference

This tool runs the real, pinned **KaTeX 0.16.22** distribution from the official npm registry. It is not a Pulse runtime dependency, a rendering fallback, or part of the installer. The version is fixed for reproducibility rather than chosen as a claim about the latest release.

## Run

Requires Node.js 18 or newer and `tar` on `PATH` (Windows includes `tar.exe`). From the repository root:

```powershell
node tools/math_reference/run.mjs
# Once the archive is cached, prohibit all downloads:
node tools/math_reference/run.mjs --offline
# Explicitly refresh the reviewable reference baseline:
node tools/math_reference/run.mjs --offline --update-snapshot
# After the native --compat run has emitted its TSV:
node tools/math_reference/compare.mjs
# Or provide an explicit native report:
node tools/math_reference/compare.mjs build/math-native-compat.tsv
# Preserve the actual native/reference comparison alongside the compatibility document:
node tools/math_reference/compare.mjs --update-snapshot
# Additional actual-reference scope probes, independent of the main corpus:
node tools/math_reference/local-scope-probe.mjs
```

The initial run downloads only the archive listed in `manifest.json`, verifies its pinned SHA-512 integrity, validates archive paths, and extracts into ignored `build/math_reference_cache/`. No npm lifecycle scripts run, no global package is installed, and no product code or font assets are added. Each run revalidates the archive and extracts the pinned contents. A network or integrity failure stops the tool; it never invents reference output.

The manifest records the upstream release, npm metadata and archive URLs, integrity, license, options and corpus provenance. KaTeX is MIT licensed; the complete upstream `LICENSE` remains beside its development-only cached files. Upstream code/fonts must remain in this ignored cache, outside product build and installer inputs.

## Inputs and outputs

`corpus.tsv` is UTF-8 with four tab-separated columns: `id`, `display` (`0` or `1`), `expectation`, `source`. The source is a single line of TeX with literal backslashes, no surrounding Markdown delimiters and no TSV escape processing. Cases are original combinations of standard textbook/research mathematics, not copied book or paper excerpts.

- `native_accept`: Pulse's intended bounded subset accepts the complete formula. This is a test contract, not a claim that this Node tool tested Pulse.
- `native_fallback`: valid reference mathematics intentionally outside the native subset; Pulse must retain readable source rather than partially render it.
- `invalid`: malformed or unknown syntax expected to be rejected by both the native parser and the pinned reference under the recorded options.

The exact-source entries in `reference-limitations.json` are explicit exceptions to assuming a valid native contract is supported by the pinned reference. KaTeX 0.16.22 does not implement optional first-argument defaults in `newcommand` (confirmed by actual errors and the upstream `src/macros.js` TODO). The two optional-default inputs remain `native_accept`; their real KaTeX rejection is preserved and compared as `native_accept_reference_limited`. No reference MathML structure pass is claimed for these cases. This is distinct from a native budget fallback or invalid syntax.

KaTeX runs `renderToString` separately for MathML and HTML+MathML, with fresh macros for every call, `throwOnError: true`, `strict: "error"`, and `trust: false`. These exact options are important: an error-colored fallback is not counted as a successful reference parse.

Generated files in the cache:

| File | Purpose |
| --- | --- |
| `results.json` | Complete MathML, HTML, errors, actual acceptance and tag counts for every case |
| `results.tsv` | Corpus fields plus `katex_accepted`, `mathml_tags`, `error` (`-` when none), `mathml_constructs`, `font_variants`, `reference_limitation`; directly consumable by native tests |
| `summary.json` | Actual engine/version/options, corpus hash and verdict totals |
| `mathml/*.mathml` | One MathML artifact for each accepted expression |
| `reference.html` | Local reference gallery with original TeX, actual KaTeX render, structural summary and theme toggle |

The gallery references the extracted local CSS and fonts; the runner checks that all CSS font URLs are local and every referenced file exists. It makes no CDN request. Asset checks do not constitute a browser screenshot check.

`--update-snapshot` additionally records `reference-results.tsv` and `reference-summary.json` in this directory. These are generated observations, not handwritten expected KaTeX results. Full HTML/MathML and downloaded assets remain ignored. Snapshots should be reviewed with corpus/manifest changes. Do not silently relabel a failing native case to get a passing run.

`compare.mjs` reads the **actual native report**, validates corpus/reference identities and ID coverage, then writes `comparison.json`, `comparison.tsv` and `comparison.md` into the cache. Its default input is `build/math-native-compat.tsv` with headers `id`, `expectation`, `native_accepted`, `width`, `height`, `baseline`, `native_structures`. The last field contains native AST counts such as `fraction:1,radical:1,environment:0,phantom:0,brace:0`; extra counts are retained as evidence. Comparisons use header names rather than fixed column positions. Missing evidence, unexpected acceptance/rejection, invalid dimensions or stable structural-count differences produce a nonzero exit code. A count difference is a review finding, not proof that either renderer is wrong.

Phantoms are counted from actual `mphantom` elements. Braces are counted from actual stretchy `mo` elements containing the over/underbrace glyph, not from ambiguous `mover`/`munder` counts that also encode labels, accents and limits. The appended `mathml_constructs` column records these observations. Middle delimiters, rule-line multiplicity and explicit length geometry are not automatically equated; native targeted tests must check them separately.

Macro cases compare expanded structures, not command-name counts. Each formula and each MathML/HTML output starts with fresh macro state; the adjacent definition/undefined-use cases and `formulaLocalMacroIsolation` summary record observed formula isolation. `font_variants` counts actual MathML attributes for information only; these are not an exact test of native font mapping or glyph appearance. Local-macro optional defaults retain their explicit reference limitations rather than substituting a fabricated expansion into the reference output.

`metrics-exceptions.json` permits a zero width only for the exact ID and source of an intentional pure vertical phantom. It still requires finite metrics, positive height and a valid baseline. Other accepted formulas keep the ordinary positive-width check. This is a narrow semantic contract, not a general relaxation of containment checks.

The optional comparison `--update-snapshot` writes `docs/markdown-math-compatibility.json`, including SHA-256 hashes of both actual input reports and all per-case observations. It preserves findings, including failures, rather than turning them into a passing baseline. Refresh this evidence after changing the native implementation and rerunning its compatibility test.

The previous 44-case architecture milestone is frozen in `history/architecture-44/`, including its original contracts and actual native/reference results. Active corpus changes do not rewrite that history. The structures milestone retains all original IDs and sources, moves four implemented fallbacks into the acceptance contract, and adds complete combinations plus explicit malformed/unsupported/budget-boundary cases.

The 83-case structures milestone is likewise frozen in `history/structures-83/`. The active 132-case local-macro/font milestone retains those IDs and sources, promotes the two implemented macro/calligraphic fallbacks only after implementation, and adds 49 combinations and boundaries. Eight deterministic macro budget fixtures are maintained by `node tools/math_reference/generate-macro-budget-cases.mjs`; rerun the real reference and review the snapshots whenever regenerating or changing them. Cases beyond native budgets remain `native_fallback` when the actual pinned reference accepts them; `[10]` macro arguments, which this KaTeX version accepts, are treated this way rather than misclassified as jointly invalid.

`local-scope-probe.mjs` is an additional independent semantic probe using the same official, pinned KaTeX 0.16.22 cached distribution, with fresh macros per probe and the manifest options. It writes the actual version, full source, complete MathML or error, and observed variable tokens to `local-scope-results.json`. Both matrix-cell and substack-row renewal produce `b,a,a`, not `b,b,a`; a macro newly defined only in the first substack row is undefined in the next row. These observations exposed scope leakage that matching structural counts could not detect. They are not added to or substituted for the stable 132-case corpus; native targeted semantic regressions must check the same behavior separately.

## Comparison limits

Reference acceptance establishes whether this particular KaTeX version/options accepted a source. Tag counts expose fractions, roots, scripts, tables and limits for structural comparison. They do **not** establish native AST equivalence, matching glyph coverage, spacing correctness, containment, selection behavior, DPI behavior or pixel equality. Native parser, rendering and visual checks remain separate. The compatibility document records these distinctions and deliberate gaps.

Sources: [pinned upstream release](https://github.com/KaTeX/KaTeX/releases/tag/v0.16.22), [pinned MIT license](https://github.com/KaTeX/KaTeX/blob/v0.16.22/LICENSE), [KaTeX options](https://katex.org/docs/options.html).
