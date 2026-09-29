# document-engine

Builds native syntax and display state for editor documents.

`@lumine-code/document-engine` is the native derived-state boundary between Superstring snapshots and the editor's JavaScript compatibility facades.

## Features

- **Zero-copy text**: reads stable Superstring snapshots through the C/POD `SnapshotLease` contract.
- **Native syntax**: parses portable Tree-sitter WASM grammars and executes SCM queries with statically linked Wasmtime.
- **Shared analysis**: keeps one revisioned syntax session and immutable query indices for every view of a buffer.
- **Asynchronous highlights**: builds bounded 128-row highlight shards on the native worker pool and shares them across display views without blocking first paint.
- **Independent views**: gives each display view its own wrapping, active folds, position mappings and viewport RenderPlans.
- **Revision safety**: rejects stale parser, injection and fold results while immediately removing scopes from edited text.
- **Lifecycle safety**: cancels and drains revision, highlight, injection and node-handle jobs and leases before a Node environment finishes teardown.

## Installation

Document Engine is an internal library delivered through an exact Git commit pinned by Lumine; it is private and is not published to the npm registry. In the flat development workspace, keep `document-engine` beside `superstring` and run `npm install` to provision the verified native dependencies and build the addon.

## Usage

One `DocumentSession` owns syntax shared by the buffer, while every `DisplayView` owns independent layout and active folds:

```js
const {DocumentSession} = require('@lumine-code/document-engine')

const session = new DocumentSession()
const view = session.createDisplayView({wrapColumn: 100, tabLength: 2})
session.setLanguage({
  languageId: 'source.js',
  runtime: 'wasm',
  wasmPath: '/path/to/javascript.wasm',
  languageSegment: 'javascript',
  queryPaths: {
    highlightsQuery: [
      '/path/to/base-highlights.scm',
      '/path/to/javascript-highlights.scm',
    ],
  },
})
const snapshot = textBuffer.getSnapshot()
try {
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
} finally {
  snapshot.destroy()
}
const highlights = session.getQueryCaptures('highlightsQuery', 0, 200)
const plan = view.buildRenderPlan(0, 200)
```

## Query and injection model

`getQueryCaptures` executes the requested query lazily against the immutable published syntax revision and caches exact range/context requests. It returns a revision-tagged `Uint32Array` with stride 9: capture-name id, pattern id, property-set id, start row and column, end row and column, and start and end UTF-16 indices. Standard `web-tree-sitter` text predicates and the editor's scope adjustments, tests, capture claims, `final`, `shy`, `_IGNORE_`, scoped configuration and injection-depth semantics are resolved natively; PCRE2 performs JavaScript-compatible UTF-16 matching, including the fleet's variable-length lookbehind patterns. `#set!`, `#is?`, `#is-not?` and no-op custom directives remain available in the property tables. Query-defined injections apply the child grammar's language scope by default; `(#set! injection.language-scope "none")` omits it, matching a dynamic injection point whose `languageScope` is `null`. Parsing eagerly executes only `injectionsQuery`; highlights, folds, indents, locals and tags are viewport/range driven.

Display highlighting uses a separate internal request/commit handshake: a worker builds missing 128-row shards, JavaScript maps the small set of `(grammarId, scopeName)` keys to the editor's stable scope ids, and native code publishes the completed shards atomically. A RenderPlan whose viewport is not fully covered remains deliberately uncolored and reports `highlightCoverageComplete: false` with `syntaxRevision: 0`; current coverage is tagged by buffer, syntax, language, injection topology and query-configuration generations. The session retains at most 32 shards and 32 MiB, coalesces identical work, and keeps no more than one active plus one latest pending request. Worker queries stop at 131,072 accepted captures, while discontiguous viewports whose scope adjustments can escape their source nodes use a revision-scoped envelope capped at 8,192 rows and 8 Mi UTF-16 units; exceeding either bound leaves that context uncolored instead of publishing an incomplete result.

Layered query results merge the root tree with every parsed injection snapshot in deterministic depth/layer order, clip captures to included ranges, preserve per-layer grammar identity and stable node handles, apply `coverShallowerScopes`, and optionally synthesize base language scopes. While a newer buffer revision is awaiting syntax, queries read the lease paired with the published tree, project unchanged ranges through accepted edits and omit every capture intersecting changed text. Query-defined injection aliases are resolved through a revision-tagged registry round trip; only portable grammar descriptors cross into native code, while ranges remain native and are parsed as child layers after resolution.

Consecutive incremental revisions reconcile freshly discovered injection layers against projected prior ranges. Public layer ids remain publication-scoped, while a private reuse slot preserves an exclusively owned child parser for matching layers; incompatible or range-shape-changing layers fall back independently to a full parse. Nested callback sources are scanned in bounded tokenized waves, and diagnostics report reused layers, projected ranges, incremental and full child parses, reuse fallbacks and retained child backends.

## Snapshot and lifecycle boundary

Snapshots passed to `applyRevision` must come from a compatible native Superstring build. Browser Superstring snapshots are deliberately unsupported. Every environment registers an asynchronous N-API cleanup hook; sessions, workers, syntax-node handles and their retained snapshot leases are cancelled and drained before teardown completes.

## Building and verification

The build provisions exact Tree-sitter 0.27.0 source, PCRE2 10.44 source and Wasmtime C API 48.0.1 artifacts for Windows, Linux and macOS on x64 and arm64. Every download is checked against the SHA-256 recorded in `script/native-dependencies.json`, and installation removes downloaded inputs and compiler intermediates after preserving `build/Release/document-engine.node`.

Run `npm test` for native unit tests, `npm run test:integration` for real grammar and addon-boundary coverage, and `npm run test:query-fleet` to compile and execute every declared query from every checked-out `language-*` descriptor. Build topology, ABI discovery, CI coverage and the Git-pin release sequence are documented in [`docs/packaging.md`](docs/packaging.md).

## Contributing

Got ideas to make this package better, found a bug, or want to help add new features? Just drop your thoughts on GitHub. Any feedback is welcome!
