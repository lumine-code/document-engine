'use strict'

const assert = require('node:assert/strict')
const path = require('node:path')
const test = require('node:test')
const {DocumentSession} = require('../..')
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

async function apply(session, buffer, revision) {
  const snapshot = buffer.getSnapshot()
  try {
    return await session.applyRevision(snapshot, new Uint32Array(0), revision)
  } finally {
    snapshot.destroy()
  }
}

function scopeIdsFor(result) {
  return Uint32Array.from(result.captureNames, (name) => {
    if (name === 'source.js') return 7
    if (name === 'whole') return 11
    if (name === 'identifier') return 13
    return 17
  })
}

test('publishes shared asynchronous highlight shards without blocking the first RenderPlan', async () => {
  const source = Array.from({length: 260}, (_, index) => `value${index};`).join(
    '\n',
  )
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: '(program) @whole\n(identifier) @identifier',
    },
  })
  await apply(session, buffer, 1)
  const firstView = session.createDisplayView({wrapColumn: 0})
  const secondView = session.createDisplayView({wrapColumn: 40})

  const firstPaint = firstView.buildRenderPlan(127, 129)
  assert.equal(firstPaint.syntaxRevision, 0)
  assert.equal(firstPaint.highlightCoverageComplete, false)

  const pending = session.requestHighlightCoverage(127, 129, {
    contextGeneration: 1,
    scopeConfigsByLanguage: {'source.js': {}},
  })
  const coalesced = session.requestHighlightCoverage(127, 129, {
    contextGeneration: 1,
    scopeConfigsByLanguage: {'source.js': {}},
  })
  const [result, sameResult] = await Promise.all([pending, coalesced])
  assert.equal(result.accepted, true)
  assert.equal(result.needsCommit, true)
  assert.equal(result.coverageStartRow, 0)
  assert.equal(result.coverageEndRow, 256)
  assert.equal(sameResult.requestId, result.requestId)
  assert.deepEqual(
    result.captureGrammarIds,
    result.captureNames.map(() => 'source.js'),
  )

  let drained = false
  const drain = session.drain().then(() => {
    drained = true
  })
  await new Promise((resolve) => setImmediate(resolve))
  assert.equal(drained, false)
  const commit = session.commitHighlightCoverage(
    result.requestId,
    scopeIdsFor(result),
  )
  assert.equal(commit.accepted, true)
  assert.equal(commit.published, true)
  await drain
  assert.equal(drained, true)

  for (const view of [firstView, secondView]) {
    const plan = view.buildRenderPlan(127, 129)
    assert.equal(plan.syntaxRevision, 1)
    assert.equal(plan.highlightCoverageComplete, true)
    assert.equal(plan.highlightCoverageStartRow, 127)
    assert.equal(plan.highlightCoverageEndRow, 129)
    assert.ok(
      plan.lines.every(
        (line) =>
          Array.from(line.tags).filter((tag) => tag === -11).length === 1,
      ),
    )
  }

  const cached = await session.requestHighlightCoverage(127, 129, {
    contextGeneration: 1,
    scopeConfigsByLanguage: {'source.js': {}},
  })
  assert.equal(cached.accepted, true)
  assert.equal(cached.published, true)
  assert.equal(cached.needsCommit, false)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.highlightJobsQueued, 1)
  assert.equal(diagnostics.highlightRequestsCoalesced, 1)
  assert.equal(diagnostics.highlightShardCount, 2)
  assert.equal(diagnostics.activeHighlightJobs, 0)
  session.setLanguage(null)
  for (const view of [firstView, secondView]) {
    const plain = view.buildRenderPlan(127, 129)
    assert.equal(plain.syntaxRevision, 0)
    assert.equal(plain.highlightCoverageComplete, true)
    assert.ok(plain.lines.every((line) => !Array.from(line.tags).includes(-11)))
  }
  await session.destroy()
})

test('rejects an uncommitted shard after its context is invalidated', async () => {
  const buffer = new TextBuffer('const value = 1;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const result = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(result.needsCommit, true)
  session.invalidateHighlightIndex()
  const stale = session.commitHighlightCoverage(
    result.requestId,
    scopeIdsFor(result),
  )
  assert.equal(stale.accepted, false)
  assert.equal(session.getDiagnostics().highlightShardCount, 0)
  const retried = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(retried.needsCommit, true)
  assert.equal(
    session.commitHighlightCoverage(retried.requestId, scopeIdsFor(retried))
      .accepted,
    true,
  )
  await session.destroy()
})

test('evicts unrelated shards without dropping a shard required by the committed viewport', async () => {
  const source = Array.from(
    {length: 33 * 128},
    (_, index) => `value${index};`,
  ).join('\n')
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)

  for (let shard = 0; shard < 32; shard++) {
    const start = shard * 128
    const result = await session.requestHighlightCoverage(start, start + 1, {
      contextGeneration: 1,
    })
    assert.equal(
      session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
        .accepted,
      true,
    )
  }
  assert.equal(session.getDiagnostics().highlightShardCount, 32)

  const lastStart = 32 * 128
  const combined = await session.requestHighlightCoverage(0, lastStart + 1, {
    contextGeneration: 1,
    shardStarts: Uint32Array.of(0, lastStart),
  })
  assert.equal(combined.needsCommit, true)
  assert.equal(
    session.commitHighlightCoverage(combined.requestId, scopeIdsFor(combined))
      .accepted,
    true,
  )
  assert.equal(session.getDiagnostics().highlightShardCount, 32)

  for (const start of [0, lastStart]) {
    const cached = await session.requestHighlightCoverage(start, start + 1, {
      contextGeneration: 1,
    })
    assert.equal(cached.published, true)
    assert.equal(cached.needsCommit, false)
  }
  await session.destroy()
})

test('bounds highlight backpressure to one active and one latest pending request', async () => {
  const source = Array.from({length: 400}, (_, index) => `value${index};`).join(
    '\n',
  )
  const buffer = new TextBuffer(source)
  const session = new DocumentSession({workerDelayMs: 40})
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const options = {contextGeneration: 1}
  const first = session.requestHighlightCoverage(0, 1, options)
  const replaced = session.requestHighlightCoverage(128, 129, options)
  const latest = session.requestHighlightCoverage(256, 257, options)
  assert.equal(session.getDiagnostics().activeHighlightJobs, 1)
  assert.equal(session.getDiagnostics().pendingHighlightJobs, 1)
  assert.equal((await replaced).reason, 'superseded')
  const [firstResult, latestResult] = await Promise.all([first, latest])
  assert.equal(firstResult.accepted, true)
  assert.equal(latestResult.accepted, true)
  assert.equal(
    session.commitHighlightCoverage(
      firstResult.requestId,
      scopeIdsFor(firstResult),
    ).accepted,
    true,
  )
  assert.equal(
    session.commitHighlightCoverage(
      latestResult.requestId,
      scopeIdsFor(latestResult),
    ).accepted,
    true,
  )
  await session.drain()
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.activeHighlightJobs, 0)
  assert.equal(diagnostics.pendingHighlightJobs, 0)
  assert.equal(diagnostics.highlightRequestsSuperseded, 1)
  assert.equal(diagnostics.highlightJobsQueued, 2)
  await session.destroy()
})

test('cancels highlight work and resolves its waiters during ordinary destroy', async () => {
  const buffer = new TextBuffer('const value = 1;\n')
  const session = new DocumentSession({workerDelayMs: 60})
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const pending = session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  const destroyed = session.destroy()
  const result = await pending
  assert.equal(result.accepted, false)
  assert.equal(result.reason, 'destroyed')
  await destroyed
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.activeHighlightJobs, 0)
  assert.equal(diagnostics.pendingHighlightJobs, 0)
  assert.equal(diagnostics.stagedHighlightCommits, 0)
})

test('destroy discards a completed highlight batch that was not committed', async () => {
  const buffer = new TextBuffer('const value = 1;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const result = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(result.needsCommit, true)
  assert.equal(session.getDiagnostics().stagedHighlightCommits, 1)
  await session.destroy()
  assert.equal(session.getDiagnostics().stagedHighlightCommits, 0)
})

test('resolves a staged-highlight drain when language configuration clears it', async () => {
  const buffer = new TextBuffer('const value = 1;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const result = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(result.needsCommit, true)
  let drained = false
  const drain = session.drain().then(() => {
    drained = true
  })
  await new Promise((resolve) => setImmediate(resolve))
  assert.equal(drained, false)
  session.setLanguage(null)
  await drain
  assert.equal(drained, true)
  await session.destroy()
})

test('keeps a root highlight request current across a stable empty injection publish', async () => {
  const source = Array.from(
    {length: 1000},
    (_, index) => `value${index};`,
  ).join('\n')
  const buffer = new TextBuffer(source)
  const session = new DocumentSession({workerDelayMs: 20})
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const diagnostics = session.getDiagnostics()
  const tags = {
    bufferRevision: diagnostics.bufferRevision,
    syntaxRevision: diagnostics.syntaxRevision,
    languageGeneration: diagnostics.languageGeneration,
  }
  const highlights = session.requestHighlightCoverage(0, 128, {
    contextGeneration: 1,
  })
  const candidates = await session.getInjectionCandidates({
    ...tags,
    injectionPointGeneration: 1,
    grammars: [],
  })
  const injection = await session.applyInjectionResultBatch({
    ...tags,
    requestId: candidates.requestId,
    injectionPointGeneration: 1,
    batchIndex: 0,
    isFinal: true,
    results: [],
    unrecognizedLanguageNames: [],
  })
  assert.equal(injection.accepted, true)
  assert.equal(injection.topologyChanged, false)
  const afterInjection = session.getDiagnostics()
  assert.ok(
    afterInjection.injectionPublishedGeneration >
      diagnostics.injectionPublishedGeneration,
  )
  assert.equal(
    afterInjection.injectionTopologyGeneration,
    diagnostics.injectionTopologyGeneration,
  )
  const result = await highlights
  assert.equal(result.accepted, true)
  assert.equal(
    session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
      .accepted,
    true,
  )
  await session.destroy()
})

test('assigns a zero-length boundary capture to only the right-hand shard', async () => {
  const source = [
    'class X {',
    ...Array.from({length: 127}, (_, index) => `field${index};`),
    'import value from "module";',
  ].join('\n')
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(MISSING "}") @missing'},
  })
  await apply(session, buffer, 1)
  const oracle = session.getQueryCaptures('highlightsQuery')
  const captures = Array.from(
    {length: oracle.captures.length / oracle.captureStride},
    (_, index) => {
      const offset = index * oracle.captureStride
      return {
        name: oracle.captureNames[oracle.captures[offset]],
        startRow: oracle.captures[offset + 3],
        startIndex: oracle.captures[offset + 7],
        endIndex: oracle.captures[offset + 8],
      }
    },
  )
  assert.ok(
    captures.some(
      (capture) =>
        capture.name === 'missing' &&
        capture.startRow === 128 &&
        capture.startIndex === capture.endIndex,
    ),
    JSON.stringify(captures),
  )
  const result = await session.requestHighlightCoverage(127, 129, {
    contextGeneration: 1,
  })
  assert.equal(
    session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
      .accepted,
    true,
  )
  assert.equal(session.getDiagnostics().highlightZeroLengthRangeCount, 1)
  await session.destroy()
})

test('routes a capture adjusted across shards by its final range', async () => {
  const source = ['a;', ...Array.from({length: 199}, () => ''), 'b;'].join('\n')
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: `
        ((identifier) @moved
          (#eq? @moved "b")
          (#set! adjust.startAt parent.previousNamedSibling.firstNamedChild.startPosition)
          (#set! adjust.endAt parent.previousNamedSibling.firstNamedChild.endPosition))
      `,
    },
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  const oracle = session.getQueryCaptures('highlightsQuery', 0, 201, {
    resolveScopes: true,
    includeLanguageScopes: true,
    compactHighlights: true,
  })
  const movedName = oracle.captureNames.indexOf('moved')
  assert.notEqual(movedName, -1)
  assert.ok(
    Array.from(oracle.highlightRanges).some(
      (value, index, ranges) =>
        index % oracle.highlightRangeStride === 0 &&
        value === movedName &&
        ranges[index + 1] === 0 &&
        ranges[index + 2] === 1,
    ),
  )

  const right = await session.requestHighlightCoverage(128, 201, {
    contextGeneration: 1,
  })
  assert.equal(
    session.commitHighlightCoverage(right.requestId, scopeIdsFor(right))
      .accepted,
    true,
  )

  const result = await session.requestHighlightCoverage(0, 201, {
    contextGeneration: 1,
  })
  assert.equal(
    session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
      .accepted,
    true,
  )
  const plan = view.buildRenderPlan(0, 201)
  assert.equal(plan.highlightCoverageComplete, true)
  assert.ok(Array.from(plan.lines[0].tags).includes(-17))
  await session.destroy()
})

test('deduplicates cross-shard mirrors without collapsing distinct query patterns', async () => {
  const buffer = new TextBuffer('const x = 1;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: '(identifier) @dup\n(identifier) @dup',
    },
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  const result = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(
    session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
      .accepted,
    true,
  )
  const tags = Array.from(view.buildRenderPlan(0, 1).lines[0].tags)
  assert.equal(tags.filter((tag) => tag === -17).length, 2)
  await session.destroy()
})

test('keeps the synthetic language scope outside captures that start at offset zero', async () => {
  const buffer = new TextBuffer('x;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(program) @whole'},
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  const result = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(
    session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
      .accepted,
    true,
  )
  assert.deepEqual(
    Array.from(view.buildRenderPlan(0, 1).lines[0].tags),
    [-7, -11, 2, -12, -8],
  )
  await session.destroy()
})

test('queries the native envelope for outward adjustments across discontiguous fold shards', async () => {
  const lines = Array.from({length: 902}, () => '')
  lines[0] = 'a;'
  lines[500] = 'b;'
  lines[900] = 'c;'
  const buffer = new TextBuffer(lines.join('\n'))
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: `
        ((identifier) @moved
          (#eq? @moved "b")
          (#set! adjust.startAt parent.previousNamedSibling.firstNamedChild.startPosition)
          (#set! adjust.endAt parent.previousNamedSibling.firstNamedChild.endPosition))
      `,
    },
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  view.replaceFolds(1, 1, Uint32Array.of(7, 1, 0, 900, 0))
  const shardStarts = view._highlightShardStartsForScreenRows(0, 3)
  assert.deepEqual(Array.from(shardStarts), [0, 896])

  const result = await session.requestHighlightCoverage(0, 901, {
    contextGeneration: 1,
    shardStarts,
  })
  assert.equal(result.needsCommit, true)
  assert.equal(
    session.commitHighlightCoverage(result.requestId, scopeIdsFor(result))
      .accepted,
    true,
  )
  const plan = view.buildRenderPlan(0, 3)
  assert.equal(plan.highlightCoverageComplete, true)
  assert.ok(Array.from(plan.lines[0].tags).includes(-17))
  assert.equal(session.getDiagnostics().highlightShardCount, 2)

  const ordinary = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(ordinary.needsCommit, true)
  session.abortHighlightCoverage(ordinary.requestId)
  await session.destroy()
})

test('rejects an unsafe folded envelope before querying beyond the worker budget', async () => {
  const lines = Array.from({length: 9001}, () => '')
  lines[0] = 'a;'
  lines[9000] = 'b;'
  const buffer = new TextBuffer(lines.join('\n'))
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: `
        ((identifier) @moved
          (#eq? @moved "b")
          (#set! adjust.startAt parent.previousNamedSibling.firstNamedChild.startPosition)
          (#set! adjust.endAt parent.previousNamedSibling.firstNamedChild.endPosition))
      `,
    },
  })
  await apply(session, buffer, 1)
  const before = session.getDiagnostics()
  const result = await session.requestHighlightCoverage(0, 9001, {
    contextGeneration: 1,
    shardStarts: Uint32Array.of(0, 8960),
  })
  assert.equal(result.accepted, false)
  assert.equal(result.permanentIncomplete, true)
  assert.equal(result.reason, 'highlight-envelope-capacity')
  const after = session.getDiagnostics()
  assert.equal(after.queryProgramsExecuted, before.queryProgramsExecuted)
  assert.equal(after.highlightShardCount, 0)
  await session.destroy()
})

test('preserves capture order for outward adjustments across contiguous shards', async () => {
  const source = [
    '(',
    ...Array.from({length: 125}, (_, index) => `x${index},`),
    'target',
    ');',
    ...Array.from({length: 72}, () => ''),
    'b;',
  ].join('\n')
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: `
        (identifier) @generic
        ((identifier) @left (#eq? @left "target"))
        ((identifier) @moved
          (#eq? @moved "b")
          (#set! adjust.startAt parent.previousNamedSibling.firstNamedChild.firstNamedChild.lastNamedChild.startPosition)
          (#set! adjust.endAt parent.previousNamedSibling.firstNamedChild.firstNamedChild.lastNamedChild.endPosition))
      `,
    },
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  const result = await session.requestHighlightCoverage(0, 201, {
    contextGeneration: 1,
  })
  const ids = {'source.js': 7, generic: 11, left: 13, moved: 17}
  assert.equal(
    session.commitHighlightCoverage(
      result.requestId,
      Uint32Array.from(result.captureNames, (name) => ids[name] ?? 19),
    ).accepted,
    true,
  )
  assert.deepEqual(
    Array.from(view.buildRenderPlan(0, 201).lines[126].tags),
    [-7, -11, -13, -17, 6, -18, -14, -12, -8],
  )
  await session.destroy()
})

test('rejects an active highlight result after a newer text revision', async () => {
  const buffer = new TextBuffer('const before = 1;\n')
  const session = new DocumentSession({workerDelayMs: 30})
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const staleRequest = session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  buffer.setText('const after = 2;\n')
  const nextRevision = apply(session, buffer, 2)
  const stale = await staleRequest
  assert.equal(stale.accepted, false)
  assert.ok(['cancelled', 'stale-result'].includes(stale.reason))
  await nextRevision
  const current = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(current.accepted, true)
  assert.equal(current.bufferRevision, 2)
  assert.equal(
    session.commitHighlightCoverage(current.requestId, scopeIdsFor(current))
      .accepted,
    true,
  )
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.activeHighlightJobs, 0)
  assert.equal(diagnostics.stagedHighlightCommits, 0)
  await session.destroy()
})

test('keys worker highlight shards by scoped query configuration', async () => {
  const buffer = new TextBuffer('const enabled = 1;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery:
        '((identifier) @config.enabled (#is? test.config "feature true"))',
    },
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  const enabled = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
    scopeConfigsByLanguage: {'source.js': {feature: true}},
  })
  assert.ok(enabled.captureNames.includes('config.enabled'))
  assert.equal(
    session.commitHighlightCoverage(enabled.requestId, scopeIdsFor(enabled))
      .accepted,
    true,
  )
  assert.ok(Array.from(view.buildRenderPlan(0, 1).lines[0].tags).includes(-17))

  const disabled = session.requestHighlightCoverage(0, 1, {
    contextGeneration: 2,
    scopeConfigsByLanguage: {'source.js': {feature: false}},
  })
  const firstPaint = view.buildRenderPlan(0, 1)
  assert.equal(firstPaint.highlightCoverageComplete, false)
  assert.equal(firstPaint.syntaxRevision, 0)
  const disabledResult = await disabled
  assert.equal(disabledResult.captureNames.includes('config.enabled'), false)
  assert.equal(
    session.commitHighlightCoverage(
      disabledResult.requestId,
      scopeIdsFor(disabledResult),
    ).accepted,
    true,
  )
  const current = view.buildRenderPlan(0, 1)
  assert.equal(current.highlightCoverageComplete, true)
  assert.equal(current.syntaxRevision, 1)
  assert.equal(Array.from(current.lines[0].tags).includes(-17), false)
  await session.destroy()
})

test('keeps an unresolved scope query permanently incomplete instead of publishing empty coverage', async () => {
  const buffer = new TextBuffer('const value = 1;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery:
        '((identifier) @identifier (#set! adjust.startAndEndAroundFirstMatchOf "["))',
    },
  })
  await apply(session, buffer, 1)
  const view = session.createDisplayView()
  const oracle = session.getQueryCaptures('highlightsQuery', 0, 1, {
    resolveScopes: true,
    includeLanguageScopes: true,
  })
  assert.equal(oracle.resolutionComplete, false)

  const result = await session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  assert.equal(result.accepted, false)
  assert.equal(result.needsCommit, false)
  assert.equal(result.permanentIncomplete, true)
  assert.equal(result.reason, 'highlight-resolution-incomplete')
  const plan = view.buildRenderPlan(0, 1)
  assert.equal(plan.highlightCoverageComplete, false)
  assert.equal(plan.syntaxRevision, 0)
  assert.equal(session.getDiagnostics().highlightShardCount, 0)
  await session.destroy()
})

test('does not coalesce a fresh request onto cancelled same-tag work', async () => {
  const buffer = new TextBuffer('const value = 1;\n')
  const session = new DocumentSession({workerDelayMs: 50})
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: '(identifier) @identifier'},
  })
  await apply(session, buffer, 1)
  const first = session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  session.invalidateHighlightIndex()
  const fresh = session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  const stale = await first
  assert.equal(stale.accepted, false)
  const current = await fresh
  assert.equal(current.accepted, true)
  assert.notEqual(current.requestId, stale.requestId)
  assert.equal(
    session.commitHighlightCoverage(current.requestId, scopeIdsFor(current))
      .accepted,
    true,
  )
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.highlightRequestsCoalesced, 0)
  assert.equal(diagnostics.highlightJobsQueued, 2)
  await session.destroy()
})

test('clears per-view sync ranges before reporting an injection fallback', async () => {
  const buffer = new TextBuffer('const pattern = /a/;\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      highlightsQuery: '(identifier) @identifier',
      injectionsQuery: `
        ((regex_pattern) @injection.content
          (#set! injection.language "regex"))
      `,
    },
  })
  await apply(session, buffer, 1)
  assert.equal(session.getDiagnostics().injectionLayerCount, 1)
  const view = session.createDisplayView()
  view.replaceHighlightRanges(1, 1, Uint32Array.of(17, 0, 5, 1, 0))
  assert.equal(view.buildRenderPlan(0, 1).syntaxRevision, 1)

  const fallback = session.requestHighlightCoverage(0, 1, {
    contextGeneration: 1,
  })
  const uncolored = view.buildRenderPlan(0, 1)
  assert.equal(uncolored.syntaxRevision, 0)
  assert.equal(uncolored.highlightCoverageComplete, false)
  const result = await fallback
  assert.equal(result.fallbackSync, true)
  assert.equal(result.reason, 'injections-require-sync')
  assert.equal(session.getDiagnostics().highlightFallbackSync, 1)
  await session.destroy()
})
