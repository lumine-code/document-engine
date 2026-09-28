'use strict'

const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')
const test = require('node:test')
const {DocumentSession, capabilities} = require('../..')
const {loadSuperstring} = require('../helpers')

const {TextBuffer} = loadSuperstring()
const javascriptWasm = path.resolve(
  __dirname,
  '..',
  '..',
  '..',
  'language-javascript',
  'grammars',
  'javascript.wasm',
)
const typescriptWasm = path.resolve(
  __dirname,
  '..',
  '..',
  '..',
  'language-typescript',
  'grammars',
  'typescript.wasm',
)
const todoWasm = path.resolve(
  __dirname,
  '..',
  '..',
  '..',
  'language-todo',
  'grammars',
  'todo.wasm',
)

function configureJavaScript(session, wasmPath = javascriptWasm) {
  session.setLanguage({
    languageId: 'source.js',
    runtime: 'wasm',
    wasmPath,
    languageName: 'javascript',
    languageSegment: 'javascript',
    queryPaths: {
      highlights: [
        path.join(path.dirname(javascriptWasm), 'javascript-highlights.scm'),
      ],
    },
  })
}

async function apply(session, buffer, edits, revision) {
  const snapshot = buffer.getSnapshot()
  try {
    return await session.applyRevision(snapshot, edits, revision)
  } finally {
    snapshot.destroy()
  }
}

test('parses a Superstring lease through native Tree-sitter Wasm', async (t) => {
  assert.equal(fs.existsSync(javascriptWasm), true, javascriptWasm)
  assert.equal(capabilities.nativeSyntaxWasm, true)
  assert.equal(capabilities.nativeQueries, true)
  assert.equal(capabilities.nativeDisplayParity, true)
  assert.equal(capabilities.nativeGrammarCache, true)
  assert.equal(capabilities.treeSitterVersion, '0.27.0')
  assert.equal(capabilities.wasmtimeVersion, '48.0.1')
  assert.equal(capabilities.maxSyntaxUtf16Length, 0x7fffffff)

  await t.test(
    'initial and safe incremental parses stay zero-copy',
    async () => {
      const buffer = new TextBuffer('const answer = 1;\n')
      const session = new DocumentSession()
      configureJavaScript(session)

      const initial = await apply(session, buffer, new Uint32Array(0), 1)
      assert.equal(initial.accepted, true)
      assert.equal(initial.syntaxParsed, true)
      assert.equal(initial.syntaxIncremental, false)
      assert.equal(initial.syntaxRootType, 'program')
      assert.equal(initial.syntaxRootHasError, false)
      assert.match(initial.syntaxChecksum, /^[0-9a-f]{16}$/)
      assert.ok(initial.syntaxNodeCount > 1)

      buffer.setText('const answer = 1;\nanswer += 1;\n')
      const incremental = await apply(
        session,
        buffer,
        Uint32Array.of(1, 0, 1, 0, 1, 0, 2, 0),
        2,
      )
      assert.equal(incremental.accepted, true)
      assert.equal(incremental.syntaxParsed, true)
      assert.equal(incremental.syntaxIncremental, true)
      assert.notEqual(incremental.syntaxChecksum, initial.syntaxChecksum)

      const diagnostics = session.getDiagnostics()
      assert.equal(diagnostics.syntaxRevision, 2)
      assert.equal(diagnostics.syntaxIncrementalParses, 1)
      assert.equal(diagnostics.snapshotInputBytesCopied, 0)
      assert.equal(diagnostics.fullBufferMaterializations, 0)
      assert.ok(diagnostics.syntaxWasmBytes > 0)
      assert.ok(diagnostics.grammarLoadMilliseconds >= 0)
      await session.destroy()
    },
  )

  await t.test(
    'enforces the uint32 input guard without allocating a huge buffer',
    async () => {
      const buffer = new TextBuffer('12345')
      const session = new DocumentSession({maxSyntaxUtf16Length: 10})
      const view = session.createDisplayView()
      configureJavaScript(session)
      const initial = await apply(session, buffer, new Uint32Array(0), 1)
      assert.equal(initial.syntaxParsed, true)
      view.replaceHighlightRanges(1, 1, new Uint32Array([259, 0, 5, 0, 0]))

      buffer.setText('12345678901')
      const result = await apply(
        session,
        buffer,
        new Uint32Array([0, 5, 0, 5, 0, 5, 0, 11]),
        2,
      )
      assert.equal(result.accepted, true)
      assert.equal(result.syntaxParsed, false)
      assert.equal(result.syntaxDisabledReason, 'input-too-large')
      assert.equal(result.syntaxErrorCode, 'ERR_SYNTAX_INPUT_TOO_LARGE')
      const diagnostics = session.getDiagnostics()
      assert.equal(diagnostics.bufferRevision, 2)
      assert.equal(diagnostics.syntaxRevision, 0)
      assert.equal(diagnostics.syntaxUnavailableReason, 'input-too-large')
      assert.equal(
        diagnostics.syntaxLastErrorCode,
        'ERR_SYNTAX_INPUT_TOO_LARGE',
      )
      assert.equal(diagnostics.syntaxInputTooLarge, 1)
      const plan = view.buildRenderPlan(0, 2)
      assert.deepEqual(
        plan.lines.map((line) => line.lineText),
        ['12345678901'],
      )
      assert.equal(Array.from(plan.lines[0].tags).includes(-259), false)
      await session.destroy()
    },
  )

  await t.test(
    'updates line metadata when an inserted newline completes CRLF',
    async () => {
      const buffer = new TextBuffer('let a = 1;\rlet b = 2;')
      const session = new DocumentSession()
      const view = session.createDisplayView()
      configureJavaScript(session)
      await apply(session, buffer, new Uint32Array(0), 1)

      buffer.setText('let a = 1;\r\nlet b = 2;')
      const result = await apply(
        session,
        buffer,
        Uint32Array.of(0, 11, 0, 11, 0, 11, 1, 0),
        2,
      )
      assert.equal(result.syntaxIncremental, true)
      const plan = view.buildRenderPlan(0, 10)
      assert.deepEqual(
        plan.lines.map((line) => line.lineText),
        ['let a = 1;', 'let b = 2;'],
      )
      view.destroy()
      await session.destroy()
    },
  )

  await t.test(
    'publishes a new display revision before a delayed syntax revision',
    async () => {
      const buffer = new TextBuffer('const value = 1;\n')
      const session = new DocumentSession({workerDelayMs: 40})
      const view = session.createDisplayView()
      configureJavaScript(session)
      await apply(session, buffer, new Uint32Array(0), 1)
      view.replaceHighlightRanges(1, 1, new Uint32Array([259, 6, 11, 0, 0]))

      buffer.setTextInRange(
        {start: {row: 0, column: 8}, end: {row: 0, column: 8}},
        'X',
      )
      const snapshot = buffer.getSnapshot()
      const pending = session.applyRevision(
        snapshot,
        new Uint32Array([0, 8, 0, 8, 0, 8, 0, 9]),
        2,
      )
      snapshot.destroy()

      const immediate = view.buildRenderPlan(0, 2)
      assert.equal(immediate.bufferRevision, 2)
      assert.equal(immediate.syntaxRevision, 1)
      assert.equal(immediate.lines[0].lineText, 'const vaXlue = 1;')
      assert.equal(Array.from(immediate.lines[0].tags).includes(-259), false)

      await pending
      assert.equal(session.getDiagnostics().syntaxRevision, 2)
      await session.destroy()
    },
  )

  await t.test(
    'shares compiled grammars between document sessions',
    async () => {
      const probe = new DocumentSession()
      const before = probe.getDiagnostics()
      await probe.destroy()
      const buffer = new TextBuffer('export const cached = true\n')
      const session = new DocumentSession()
      configureJavaScript(session)

      const result = await apply(session, buffer, new Uint32Array(0), 1)
      assert.equal(result.syntaxParsed, true)
      assert.equal(result.grammarCacheHit, true)
      assert.equal(result.grammarLoadMilliseconds, 0)
      const diagnostics = session.getDiagnostics()
      assert.equal(diagnostics.grammarCacheHits, 1)
      assert.equal(
        diagnostics.grammarCacheCompilations,
        before.grammarCacheCompilations,
      )
      await session.destroy()
    },
  )

  await t.test(
    'keeps the Wasm export name separate from the query language segment',
    async () => {
      assert.equal(fs.existsSync(typescriptWasm), true, typescriptWasm)
      const buffer = new TextBuffer('const value: number = 1\n')
      const session = new DocumentSession()
      session.setLanguage({
        languageId: 'source.ts',
        runtime: 'wasm',
        wasmPath: typescriptWasm,
        languageName: 'typescript',
        languageSegment: 'ts',
      })

      const result = await apply(session, buffer, new Uint32Array(0), 1)
      assert.equal(result.syntaxParsed, true)
      assert.equal(result.syntaxRootType, 'program')
      assert.equal(result.syntaxRootHasError, false)
      const diagnostics = session.getDiagnostics()
      assert.equal(diagnostics.languageName, 'typescript')
      assert.equal(diagnostics.languageSegment, 'ts')
      await session.destroy()
    },
  )

  await t.test(
    'discovers a mismatched language export from the Wasm export section',
    async () => {
      assert.equal(fs.existsSync(todoWasm), true, todoWasm)
      const buffer = new TextBuffer('TODO native query migration\n')
      const session = new DocumentSession()
      session.setLanguage({
        languageId: 'text.todo',
        runtime: 'wasm',
        wasmPath: todoWasm,
        languageName: 'todo',
      })

      const result = await apply(session, buffer, new Uint32Array(0), 1)
      assert.equal(result.syntaxParsed, true)
      assert.equal(result.resolvedLanguageName, 'TODO')
      assert.equal(session.getDiagnostics().resolvedLanguageName, 'TODO')
      assert.match(session.getDiagnostics().grammarFingerprint, /todo\.wasm/i)
      await session.destroy()
    },
  )

  await t.test(
    'fails open to plain text for invalid grammar Wasm with a stable diagnostic',
    async () => {
      const buffer = new TextBuffer('const value = 1')
      const session = new DocumentSession()
      const view = session.createDisplayView()
      configureJavaScript(session, __filename)

      const result = await apply(session, buffer, new Uint32Array(0), 1)
      assert.equal(result.accepted, true)
      assert.equal(result.syntaxParsed, false)
      assert.equal(result.syntaxDisabledReason, 'grammar-error')
      assert.equal(result.syntaxErrorCode, 'ERR_SYNTAX_WASM_LOAD_FAILED')
      const diagnostics = session.getDiagnostics()
      assert.equal(diagnostics.syntaxRevision, 0)
      assert.equal(diagnostics.syntaxUnavailableReason, 'grammar-error')
      assert.equal(
        diagnostics.syntaxLastErrorCode,
        'ERR_SYNTAX_WASM_LOAD_FAILED',
      )
      assert.equal(diagnostics.syntaxFailOpenCount, 1)
      assert.match(diagnostics.syntaxLastError, /Wasm/i)
      assert.deepEqual(
        view.buildRenderPlan(0, 2).lines.map((line) => line.lineText),
        ['const value = 1'],
      )
      await session.destroy()
    },
  )

  await t.test('cancels parsing and drains before teardown', async () => {
    const buffer = new TextBuffer('const value = 1;\n'.repeat(20000))
    const session = new DocumentSession({workerDelayMs: 20})
    configureJavaScript(session)
    const revision = apply(session, buffer, new Uint32Array(0), 1)
    const destroyed = session.destroy()

    await assert.rejects(
      revision,
      (error) => error.code === 'ERR_DOCUMENT_SESSION_DESTROYED',
    )
    await destroyed
    const diagnostics = session.getDiagnostics()
    assert.equal(diagnostics.activeJobs, 0)
    assert.equal(diagnostics.pendingJobs, 0)
  })
})
