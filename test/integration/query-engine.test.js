'use strict'

const assert = require('node:assert/strict')
const fs = require('node:fs')
const os = require('node:os')
const path = require('node:path')
const test = require('node:test')
const {DocumentSession, capabilities} = require('../..')
const {loadSuperstring} = require('../helpers')

const {TextBuffer} = loadSuperstring()
const workspaceRoot = path.resolve(__dirname, '..', '..', '..')
const javascriptWasm = path.join(
  workspaceRoot,
  'language-javascript',
  'grammars',
  'javascript.wasm',
)
const pythonWasm = path.join(
  workspaceRoot,
  'language-python',
  'grammars',
  'python.wasm',
)
const htmlWasm = path.join(
  workspaceRoot,
  'language-html',
  'grammars',
  'html.wasm',
)
const markdownWasm = path.join(
  workspaceRoot,
  'language-gfm',
  'grammars',
  'markdown.wasm',
)

async function apply(session, text, revision = 1) {
  const buffer = new TextBuffer(text)
  const snapshot = buffer.getSnapshot()
  try {
    return await session.applyRevision(snapshot, new Uint32Array(0), revision)
  } finally {
    snapshot.destroy()
  }
}

function unpackCaptures(result) {
  const output = []
  for (
    let offset = 0;
    offset < result.captures.length;
    offset += result.captureStride
  ) {
    output.push({
      name: result.captureNames[result.captures[offset]],
      patternIndex: result.captures[offset + 1],
      propertySetId: result.captures[offset + 2],
      startRow: result.captures[offset + 3],
      startColumn: result.captures[offset + 4],
      endRow: result.captures[offset + 5],
      endColumn: result.captures[offset + 6],
      startIndex: result.captures[offset + 7],
      endIndex: result.captures[offset + 8],
    })
  }
  return output
}

let webTreeSitterPromise
async function loadWebTreeSitter() {
  if (!webTreeSitterPromise) {
    webTreeSitterPromise = (async () => {
      const TreeSitter = require('web-tree-sitter')
      await TreeSitter.Parser.init()
      return TreeSitter
    })()
  }
  return webTreeSitterPromise
}

async function webCaptures(wasmPath, source, querySource) {
  const TreeSitter = await loadWebTreeSitter()
  const language = await TreeSitter.Language.load(wasmPath)
  const parser = new TreeSitter.Parser()
  parser.setLanguage(language)
  const tree = parser.parse(source)
  const query = new TreeSitter.Query(language, querySource)
  try {
    return query.captures(tree.rootNode).map((capture) => ({
      name: capture.name,
      startIndex: capture.node.startIndex,
      endIndex: capture.node.endIndex,
    }))
  } finally {
    query.delete()
    tree.delete()
    parser.delete()
  }
}

test('does not reuse same-shape syntax after an empty-edit resynchronization', async () => {
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: '((identifier) @matched (#eq? @matched "abc"))',
    },
  })
  const buffer = new TextBuffer('abc')
  let snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()
  assert.equal(
    unpackCaptures(session.getQueryCaptures('highlightsQuery')).length,
    1,
  )

  buffer.setText('xyz')
  snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 2)
  snapshot.destroy()
  assert.equal(
    unpackCaptures(session.getQueryCaptures('highlightsQuery')).length,
    0,
  )
  await session.destroy()
})

test('matches web-tree-sitter text predicates and preserves property metadata', async () => {
  const source = 'const keep = drop;\nkeep + other;\n'
  const querySource = String.raw`
    ((identifier) @equal.keep
      (#eq? @equal.keep "keep")
      (#set! capture.final)
      (#is? test.first true))
    ((identifier) @not.drop
      (#not-eq? @not.drop "drop"))
    ((identifier) @matches.k
      (#match? @matches.k "^k"))
    ((identifier) @one.of
      (#any-of? @one.of "keep" "other"))
    ((identifier) @custom.metadata
      (#custom-predicate! @custom.metadata "payload"))
  `
  const expected = await webCaptures(javascriptWasm, source, querySource)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: querySource},
  })
  await apply(session, source)

  const result = session.getQueryCaptures('highlightsQuery', 0, 0xffffffff, {
    resolveScopes: false,
  })
  const actual = unpackCaptures(result).map((capture) => ({
    name: capture.name,
    startIndex: capture.startIndex,
    endIndex: capture.endIndex,
  }))
  assert.deepEqual(actual, expected)
  assert.equal(result.bufferRevision, 1)
  assert.equal(result.syntaxRevision, 1)
  assert.equal(result.captureStride, 9)
  const compact = session.getQueryCaptures('highlightsQuery', 0, 0xffffffff, {
    resolveScopes: false,
    compactHighlights: true,
  })
  assert.deepEqual(
    Array.from(compact.highlightRanges),
    Array.from(result.highlightRanges),
  )
  assert.deepEqual(
    Array.from(compact.highlightLayerIndices),
    Array.from(result.highlightLayerIndices),
  )
  assert.deepEqual(compact.captureNames, result.captureNames)
  assert.deepEqual(
    compact.highlightGrammarIds,
    result.layers.map((layer) => layer.grammarId),
  )
  assert.equal(compact.captures.length, 0)
  assert.equal(compact.captureLayerIndices.length, 0)
  assert.equal(compact.captureDepths.length, 0)
  assert.equal(compact.captureOrders.length, 0)
  assert.equal(compact.captureFlags.length, 0)
  assert.equal(compact.captureNodeHandles.length, 0)
  assert.equal(compact.highlightFlags.length, 0)
  assert.equal(compact.highlightNodeHandles.length, 0)
  assert.deepEqual(compact.propertySets, [])
  assert.deepEqual(compact.layers, [])
  assert.equal(compact.rawCaptureCount, result.rawCaptureCount)
  assert.equal(compact.acceptedCaptureCount, result.acceptedCaptureCount)
  assert.throws(
    () =>
      session.getQueryCaptures('foldsQuery', 0, 1, {
        compactHighlights: true,
      }),
    (error) => error.code === 'ERR_INVALID_QUERY_CONTEXT',
  )
  assert.ok(
    unpackCaptures(session.getQueryCaptures('highlightsQuery', 1, 2)).every(
      (capture) => capture.startRow === 1,
    ),
  )
  const sameRow = unpackCaptures(
    session.getQueryCaptures('highlightsQuery', 0, 0, {
      resolveScopes: false,
      startColumn: 6,
      endColumn: 10,
    }),
  )
  assert.ok(sameRow.length > 0)
  assert.ok(
    sameRow.every(
      (capture) =>
        capture.startRow === 0 &&
        capture.startColumn >= 6 &&
        capture.endRow === 0 &&
        capture.endColumn <= 10,
    ),
  )
  assert.throws(
    () =>
      session.getQueryCaptures('highlightsQuery', 0, 0, {
        startColumn: 10,
        endColumn: 6,
      }),
    (error) => error.code === 'ERR_INVALID_QUERY_RANGE',
  )

  const equalCapture = unpackCaptures(result).find(
    (capture) => capture.name === 'equal.keep',
  )
  assert.ok(equalCapture)
  assert.deepEqual(result.propertySets[equalCapture.propertySetId].set, {
    'capture.final': null,
  })
  assert.deepEqual(result.propertySets[equalCapture.propertySetId].asserted, {
    'test.first': 'true',
  })
  const customCapture = unpackCaptures(result).find(
    (capture) => capture.name === 'custom.metadata',
  )
  assert.deepEqual(
    result.propertySets[customCapture.propertySetId].predicates,
    [
      {
        operator: 'custom-predicate!',
        operands: [
          {type: 'capture', value: 'custom.metadata'},
          {type: 'string', value: 'payload'},
        ],
      },
    ],
  )
  const diagnostics = session.getDiagnostics()
  assert.ok(diagnostics.queryTextPredicates >= 4)
  assert.ok(diagnostics.queryCustomPredicates >= 1)
  assert.equal(diagnostics.queryUnresolvedPredicates, 0)
  assert.equal(capabilities.nativeQueries, true)
  await session.destroy()
})

test('concatenates query paths in descriptor order and substitutes languageSegment', async (t) => {
  const directory = fs.mkdtempSync(
    path.join(os.tmpdir(), 'lumine-document-query-'),
  )
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}))
  const first = path.join(directory, 'first.scm')
  const second = path.join(directory, 'second.scm')
  fs.writeFileSync(first, '(identifier) @variable._LANG_\n')
  fs.writeFileSync(second, '(identifier) @support._LANG_\n')

  const session = new DocumentSession()
  session.setLanguage({
    languageId: 'source.js',
    runtime: 'wasm',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    languageSegment: 'js',
    queryPaths: {highlightsQuery: [first, second]},
  })
  await apply(session, 'const value = 42\n')
  const result = session.getQueryCaptures('highlightsQuery')
  const names = unpackCaptures(result).map((capture) => capture.name)
  assert.ok(names.includes('variable.js'))
  assert.ok(names.includes('support.js'))
  assert.ok(
    names.indexOf('variable.js') < names.indexOf('support.js'),
    'captures retain concatenated query pattern order for the same source span',
  )
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.queryFileCount, 2)
  assert.equal(diagnostics.queryLanguageSegmentSubstitutions, 2)
  assert.equal(diagnostics.queryLanguageSegmentWarnings, 0)
  await session.destroy()
})

test('reports scoped configuration dependencies without executing a query', async () => {
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: String.raw`
        ((identifier) @configured
          (#is? test.config "feature.enabled true")
          (#is-not? test.config "feature.mode loose"))
        ((identifier) @configured.again
          (#set! test.config "feature.enabled true"))
      `,
    },
  })
  await apply(session, 'const value = 1\n')
  assert.deepEqual(
    session.getQueryRequirements('highlightsQuery').scopeConfigKeys,
    ['feature.enabled', 'feature.mode'],
  )
  assert.equal(session.getDiagnostics().queryProgramsExecuted, 0)
  await session.destroy()
})

test('builds revision-tagged private indices for representative JavaScript and Python queries', async () => {
  const cases = [
    {
      id: 'source.js',
      wasmPath: javascriptWasm,
      languageName: 'javascript',
      source: 'export function answer (value) { return value + 42 }\n',
      paths: {
        highlightsQuery: [
          path.join(
            workspaceRoot,
            'language-javascript',
            'grammars',
            'javascript-highlights.scm',
          ),
        ],
        foldsQuery: [
          path.join(
            workspaceRoot,
            'language-javascript',
            'grammars',
            'javascript-folds.scm',
          ),
        ],
        indentsQuery: [
          path.join(
            workspaceRoot,
            'language-javascript',
            'grammars',
            'javascript-indents.scm',
          ),
        ],
        localsQuery: [
          path.join(
            workspaceRoot,
            'language-javascript',
            'grammars',
            'javascript-locals.scm',
          ),
        ],
        tagsQuery: [
          path.join(
            workspaceRoot,
            'language-javascript',
            'grammars',
            'javascript-tags.scm',
          ),
        ],
      },
    },
    {
      id: 'source.python',
      wasmPath: pythonWasm,
      languageName: 'python',
      source: 'class Answer:\n    def value(self):\n        return 42\n',
      paths: {
        highlightsQuery: [
          path.join(
            workspaceRoot,
            'language-python',
            'grammars',
            'python-highlights.scm',
          ),
        ],
        foldsQuery: [
          path.join(
            workspaceRoot,
            'language-python',
            'grammars',
            'python-folds.scm',
          ),
        ],
        indentsQuery: [
          path.join(
            workspaceRoot,
            'language-python',
            'grammars',
            'python-indents.scm',
          ),
        ],
        tagsQuery: [
          path.join(
            workspaceRoot,
            'language-python',
            'grammars',
            'python-tags.scm',
          ),
        ],
      },
    },
  ]

  for (const entry of cases) {
    const session = new DocumentSession()
    session.setLanguage({
      languageId: entry.id,
      runtime: 'wasm',
      wasmPath: entry.wasmPath,
      languageName: entry.languageName,
      queryPaths: entry.paths,
    })
    await apply(session, entry.source)
    const highlights = session.getQueryCaptures('highlightsQuery', 0, 2)
    assert.equal(highlights.bufferRevision, 1)
    assert.equal(highlights.languageGeneration, 1)
    assert.ok(highlights.captures.length > 0, entry.id)
    assert.ok(session.getDiagnostics().queryProgramsExecuted >= 1, entry.id)
    await session.destroy()
  }
})

test('indexes query-based HTML and GFM injection candidates while JS callbacks remain outside native parity', async () => {
  const cases = [
    {
      id: 'text.html.basic',
      wasmPath: htmlWasm,
      languageName: 'html',
      source: '<script>const answer = 42</script>',
      query: String.raw`
        (script_element
          (raw_text) @injection.content
          (#set! injection.language "javascript"))
      `,
    },
    {
      id: 'source.gfm',
      wasmPath: markdownWasm,
      languageName: 'markdown',
      source: '```javascript\nconst answer = 42\n```\n',
      query: String.raw`
        (fenced_code_block
          (info_string (language) @injection.language)
          (code_fence_content) @injection.content)
      `,
    },
  ]
  for (const entry of cases) {
    const session = new DocumentSession()
    session.configureSyntax({
      languageId: entry.id,
      wasmPath: entry.wasmPath,
      languageName: entry.languageName,
      queries: {injectionsQuery: entry.query},
    })
    await apply(session, entry.source)
    const captures = unpackCaptures(session.getQueryCaptures('injectionsQuery'))
    assert.ok(
      captures.some((capture) => capture.name === 'injection.content'),
      entry.id,
    )
    assert.ok(
      captures.some((capture) => capture.name === 'injection.language') ||
        entry.id === 'text.html.basic',
      entry.id,
    )
    await session.destroy()
  }
  assert.equal(capabilities.nativeQueries, true)
})

test('maps malformed concatenated queries back to the offending file and line', async (t) => {
  const directory = fs.mkdtempSync(
    path.join(os.tmpdir(), 'lumine-document-query-error-'),
  )
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}))
  const valid = path.join(directory, 'valid.scm')
  const invalid = path.join(directory, 'invalid.scm')
  fs.writeFileSync(valid, '(identifier) @valid\n')
  fs.writeFileSync(
    invalid,
    '; first line\n(definitely_not_a_javascript_node) @invalid\n',
  )

  const session = new DocumentSession()
  session.setLanguage({
    languageId: 'source.js',
    runtime: 'wasm',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queryPaths: {highlightsQuery: [valid, invalid]},
  })
  const result = await apply(session, 'const value = 1\n')
  assert.equal(result.accepted, true)
  assert.equal(result.syntaxParsed, false)
  assert.equal(result.syntaxDisabledReason, 'query-error')
  assert.equal(result.syntaxErrorCode, 'ERR_SYNTAX_QUERY_COMPILE_FAILED')
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.syntaxRevision, 0)
  assert.equal(diagnostics.syntaxUnavailableReason, 'query-error')
  assert.equal(
    diagnostics.queryLastErrorCode,
    'ERR_SYNTAX_QUERY_COMPILE_FAILED',
  )
  assert.equal(
    path.resolve(diagnostics.queryLastErrorPath),
    path.resolve(invalid),
  )
  assert.equal(diagnostics.queryLastErrorLine, 2)
  assert.match(diagnostics.queryLastError, /highlightsQuery/)
  await session.destroy()
})

test('does not crash on a valid JavaScript regexp unsupported by std::regex', async () => {
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: String.raw`((identifier) @lookbehind (#match? @lookbehind "(?<=keep)$"))`,
    },
  })
  await apply(session, 'const keep = other\n')
  assert.ok(session.getQueryCaptures('highlightsQuery').captures.length > 0)
  assert.equal(session.getDiagnostics().queryUnresolvedPredicates, 0)
  assert.equal(session.getDiagnostics().queryUnresolvedRegexPredicates, 0)
  assert.equal(capabilities.nativeQueries, true)
  await session.destroy()
})

test('shares immutable query programs process-wide for the same grammar generation', async () => {
  const query = '(identifier) @cache.unique_query_program_20260928\n'
  const first = new DocumentSession()
  first.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: query},
  })
  await apply(first, 'const first = 1\n')

  const second = new DocumentSession()
  second.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: query},
  })
  await apply(second, 'const second = 2\n')
  const diagnostics = second.getDiagnostics()
  assert.equal(capabilities.nativeQueryCache, true)
  assert.equal(diagnostics.queryCacheHits, 1)
  assert.equal(diagnostics.queryProgramsCompiled, 0)
  assert.equal(diagnostics.queryProgramsExecuted, 0)
  second.getQueryCaptures('highlightsQuery')
  assert.equal(second.getDiagnostics().queryProgramsExecuted, 1)
  await first.destroy()
  await second.destroy()
})

test('separates query-cache entries when the grammar module fingerprint changes', async (t) => {
  const directory = fs.mkdtempSync(
    path.join(os.tmpdir(), 'lumine-query-grammar-fingerprint-'),
  )
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}))
  const wasmPath = path.join(directory, 'javascript.wasm')
  fs.copyFileSync(javascriptWasm, wasmPath)
  const query = '(identifier) @cache.fingerprint_unique_20260928\n'

  const configure = (session) =>
    session.configureSyntax({
      languageId: 'source.js',
      wasmPath,
      languageName: 'javascript',
      queries: {highlightsQuery: query},
    })
  const first = new DocumentSession()
  configure(first)
  await apply(first, 'const first = 1\n')
  const firstFingerprint = first.getDiagnostics().grammarFingerprint
  await first.destroy()

  const future = new Date(Date.now() + 60_000)
  fs.utimesSync(wasmPath, future, future)
  const second = new DocumentSession()
  configure(second)
  await apply(second, 'const second = 2\n')
  const diagnostics = second.getDiagnostics()
  assert.notEqual(diagnostics.grammarFingerprint, firstFingerprint)
  assert.equal(diagnostics.queryCacheMisses, 1)
  assert.equal(diagnostics.queryCacheHits, 0)
  await second.destroy()
})
