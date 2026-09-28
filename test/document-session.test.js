'use strict'

const assert = require('node:assert/strict')
const path = require('node:path')
const test = require('node:test')
const {once} = require('node:events')
const {Worker} = require('node:worker_threads')
const {DocumentSession, capabilities} = require('..')
const {loadSuperstring} = require('./helpers')

const superstring = loadSuperstring()
const {TextBuffer, _getSnapshotLeaseDiagnostics} = superstring

function applyText(session, buffer, revision, foldUpdates) {
  const snapshot = buffer.getSnapshot()
  const promise = session.applyRevision(
    snapshot,
    new Uint32Array(0),
    revision,
    foldUpdates,
  )
  snapshot.destroy()
  return promise
}

test('publishes a leased snapshot without materializing the full buffer', async () => {
  const buffer = new TextBuffer('alpha\r\nbeta\n😀')
  const session = new DocumentSession()

  const result = await applyText(session, buffer, 1)
  assert.equal(result.accepted, true)
  assert.equal(result.bufferRevision, 1)
  assert.match(result.checksum, /^[0-9a-f]{16}$/)

  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.bufferRevision, 1)
  assert.equal(diagnostics.revisionsPublished, 1)
  assert.equal(diagnostics.fullBufferMaterializations, 0)
  assert.equal(diagnostics.snapshotInputBytesCopied, 0)
  assert.ok(diagnostics.snapshotLinesRead >= 1)
  assert.equal(diagnostics.activeJobs, 0)
  assert.equal(diagnostics.syntaxParses, 0)

  await session.destroy()
})

test('publishes plain text without queueing the syntax worker', async () => {
  const buffer = new TextBuffer('x'.repeat(250000))
  const session = new DocumentSession()
  const revision = applyText(session, buffer, 1)

  const immediate = session.getDiagnostics()
  assert.equal(immediate.bufferRevision, 1)
  assert.equal(immediate.activeJobs, 0)
  assert.equal(immediate.pendingJobs, 0)
  const result = await revision
  assert.equal(result.accepted, true)
  assert.equal(result.syntaxRevision, 0)
  assert.equal(result.syntaxParsed, false)
  assert.equal(session.getDiagnostics().syntaxParses, 0)
  await session.destroy()
})

test('falls back to base SnapshotLease chunk analysis without the line-index extension', async () => {
  const buffer = new TextBuffer('one\r\ntwo\n')
  const session = new DocumentSession({disableSnapshotLineIndex: true})
  await applyText(session, buffer, 1)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.synchronousLineIndexAnalyses, 0)
  assert.equal(diagnostics.synchronousFullAnalyses, 1)
  assert.ok(diagnostics.snapshotChunksRead > 0)
  assert.equal(diagnostics.snapshotLinesRead, 0)
  await session.destroy()
})

test('keeps at most one active and one latest pending revision', async () => {
  const buffer = new TextBuffer('one')
  const session = new DocumentSession({workerDelayMs: 25})

  const first = applyText(session, buffer, 1)
  buffer.setText('two')
  const second = applyText(session, buffer, 2)
  buffer.setText('three')
  const third = applyText(session, buffer, 3)

  await assert.rejects(
    second,
    (error) => error.code === 'ERR_REVISION_SUPERSEDED',
  )
  assert.equal((await first).accepted, false)
  assert.equal((await third).accepted, true)
  await session.drain()

  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.bufferRevision, 3)
  assert.equal(diagnostics.activeJobs, 0)
  assert.equal(diagnostics.pendingJobs, 0)
  assert.equal(diagnostics.revisionsSuperseded, 1)
  assert.equal(diagnostics.revisionsStale, 1)

  await session.destroy()
})

test('validates the cross-addon SnapshotLease type tag', () => {
  const session = new DocumentSession()
  assert.throws(
    () => session.applyRevision({}, new Uint32Array(0), 1),
    (error) => error.code === 'ERR_SNAPSHOT_TYPE_MISMATCH',
  )
  void session.destroy()
})

test('stores a portable grammar descriptor for the native Wasm backend', () => {
  const session = new DocumentSession()
  session.setLanguage({
    languageId: 'source.js',
    runtime: 'wasm',
    wasmPath: 'tree-sitter-javascript.wasm',
    languageSegment: 'javascript',
    queryPaths: {
      highlights: ['highlights.scm'],
      folds: ['folds.scm'],
    },
  })

  const diagnostics = session.getDiagnostics()
  assert.equal(capabilities.nativeSyntaxWasm, true)
  assert.equal(capabilities.nativeQueries, true)
  assert.equal(diagnostics.languageId, 'source.js')
  assert.equal(diagnostics.runtime, 'wasm')
  assert.equal(diagnostics.queryFileCount, 2)
  assert.equal(diagnostics.syntaxBackend, 'tree-sitter-0.27.0+wasmtime-48.0.1')
  assert.equal(diagnostics.syntaxUnavailableReason, '')

  session.setLanguage(null)
  assert.equal(session.getDiagnostics().languageId, '')
  void session.destroy()
})

test('drains an active worker before destruction completes', async () => {
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 0)
  const buffer = new TextBuffer('still leased')
  const session = new DocumentSession({workerDelayMs: 1000})
  const revision = applyText(session, buffer, 1)
  assert.ok(_getSnapshotLeaseDiagnostics().activeConsumerLeases > 0)
  const destructionStartedAt = Date.now()
  const destroyed = session.destroy()

  await assert.rejects(
    revision,
    (error) => error.code === 'ERR_DOCUMENT_SESSION_DESTROYED',
  )
  await destroyed
  assert.ok(
    Date.now() - destructionStartedAt < 500,
    'destroy() should signal cancellation before draining the native worker',
  )
  assert.equal(session.getDiagnostics().activeJobs, 0)
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 0)
})

test('releases the published snapshot lease on ordinary destroy', async () => {
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 0)
  const buffer = new TextBuffer('published lease')
  const session = new DocumentSession()
  await applyText(session, buffer, 1)
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 1)

  await session.destroy()
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 0)
})

test('drains active native work before a worker environment is torn down', async () => {
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 0)
  const superstringPath = process.env.SUPERSTRING_PATH
    ? path.resolve(process.env.SUPERSTRING_PATH)
    : path.resolve(__dirname, '..', '..', 'superstring')
  const documentEnginePath = path.resolve(__dirname, '..')
  const javascriptWasm = path.resolve(
    __dirname,
    '..',
    '..',
    'language-javascript',
    'grammars',
    'javascript.wasm',
  )
  const source = String.raw`
    const {parentPort, workerData} = require('node:worker_threads')
    const {DocumentSession} = require(workerData.documentEnginePath)
    const superstring = require(workerData.superstringPath)
    void (async () => {
      const buffer = new superstring.TextBuffer('const value = 1;\n')
      const session = new DocumentSession({workerDelayMs: 250})
      session.setLanguage({
        languageId: 'source.js',
        runtime: 'wasm',
        wasmPath: workerData.javascriptWasm,
        languageName: 'javascript'
      })
      let snapshot = buffer.getSnapshot()
      await session.applyRevision(snapshot, new Uint32Array(0), 1)
      snapshot.destroy()
      globalThis.retainedSyntaxNode = session.getSyntaxNodeAtPosition({
        row: 0,
        column: 6
      }).node

      buffer.setText('const value = 2;\n')
      snapshot = buffer.getSnapshot()
      void session.applyRevision(snapshot, new Uint32Array(0), 2).catch(() => {})
      snapshot.destroy()
      parentPort.postMessage({
        activeJobs: session.getDiagnostics().activeJobs,
        activeConsumerLeases:
          superstring._getSnapshotLeaseDiagnostics().activeConsumerLeases
      })
    })().catch(error => {
      setImmediate(() => { throw error })
    })
  `
  const worker = new Worker(source, {
    eval: true,
    workerData: {documentEnginePath, superstringPath, javascriptWasm},
  })
  const [started] = await once(worker, 'message')
  assert.equal(started.activeJobs, 1)
  assert.ok(started.activeConsumerLeases > 0)

  const terminationStartedAt = Date.now()
  await worker.terminate()
  assert.ok(
    Date.now() - terminationStartedAt < 5000,
    'native cleanup should cancel the delayed worker instead of waiting for it',
  )
  assert.equal(_getSnapshotLeaseDiagnostics().activeConsumerLeases, 0)
})
