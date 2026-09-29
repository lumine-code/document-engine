'use strict'

const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')
const test = require('node:test')
const {DocumentSession} = require('../..')
const {loadSuperstring} = require('../helpers')
const {
  DynamicInjectionBridge,
} = require('../../../lumine/src/dynamic-injection-bridge')

const {TextBuffer} = loadSuperstring()
const workspaceRoot = path.resolve(__dirname, '..', '..', '..')
const grammarPath = (packageName, fileName) =>
  path.join(workspaceRoot, packageName, 'grammars', fileName)
const javascriptWasm = grammarPath('language-javascript', 'javascript.wasm')
const javascriptInjections = grammarPath(
  'language-javascript',
  'javascript-injections.scm',
)
const regexWasm = grammarPath('language-regex', 'regex.wasm')
const regexHighlights = grammarPath('language-regex', 'regex-highlights.scm')
const htmlWasm = grammarPath('language-html', 'html.wasm')
const todoWasm = grammarPath('language-todo', 'todo.wasm')

function descriptor(languageId, wasmPath, languageName, queryPaths = {}) {
  return {
    languageId,
    runtime: 'wasm',
    wasmPath,
    languageName,
    languageSegment: languageName,
    queryPaths,
  }
}

class FakeGrammar {
  constructor(scopeName, grammarDescriptor, injectionNames = []) {
    this.scopeName = scopeName
    this.descriptor = grammarDescriptor
    this.injectionNames = injectionNames
    this.injectionPointsByType = {}
    this.addListeners = new Set()
    this.removeListeners = new Set()
  }

  getDocumentEngineDescriptor() {
    return this.descriptor
  }

  addInjectionPoint(point) {
    ;(this.injectionPointsByType[point.type] ??= []).push(point)
    for (const listener of this.addListeners) listener(point)
  }

  onDidAddInjectionPoint(callback) {
    this.addListeners.add(callback)
    return {dispose: () => this.addListeners.delete(callback)}
  }

  onDidRemoveInjectionPoint(callback) {
    this.removeListeners.add(callback)
    return {dispose: () => this.removeListeners.delete(callback)}
  }
}

function revisionTags(session) {
  const diagnostics = session.getDiagnostics()
  return {
    bufferRevision: diagnostics.bufferRevision,
    syntaxRevision: diagnostics.syntaxRevision,
    languageGeneration: diagnostics.languageGeneration,
  }
}

async function apply(session, buffer, revision, edits = new Uint32Array(0)) {
  const snapshot = buffer.getSnapshot()
  try {
    return await session.applyRevision(snapshot, edits, revision)
  } finally {
    snapshot.destroy()
  }
}

function createBridge(session, buffer, grammar, targets) {
  return new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar,
    resolveGrammar: (name) => targets.get(name) ?? null,
    getCurrentTags: () => revisionTags(session),
  })
}

function unpack(result) {
  const captures = []
  for (
    let offset = 0, captureIndex = 0;
    offset < result.captures.length;
    offset += result.captureStride, captureIndex++
  ) {
    const layerIndex = result.captureLayerIndices[captureIndex]
    captures.push({
      name: result.captureNames[result.captures[offset]],
      startIndex: result.captures[offset + 7],
      endIndex: result.captures[offset + 8],
      layerId: result.layers[layerIndex].layerId,
      grammarId: result.layers[layerIndex].grammarId,
      nodeHandle: result.captureNodeHandles[captureIndex],
    })
  }
  return captures
}

function queryHighlights(session, endRow = 100) {
  return session.getQueryCaptures('highlightsQuery', 0, endRow, {
    resolveScopes: true,
    interpolateNames: true,
    includeLanguageScopes: true,
  })
}

function semanticCaptures(result) {
  return unpack(result)
    .map(({name, startIndex, endIndex, grammarId}) => ({
      name,
      startIndex,
      endIndex,
      grammarId,
    }))
    .sort((left, right) =>
      JSON.stringify(left).localeCompare(JSON.stringify(right)),
    )
}

function queryFixture(text) {
  const session = new DocumentSession()
  const root = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript'),
  )
  const regex = new FakeGrammar(
    'source.regexp',
    descriptor('source.regexp', regexWasm, 'regex', {
      highlightsQuery: [regexHighlights],
    }),
    ['regex', 'regexp'],
  )
  const buffer = new TextBuffer(text)
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      injectionsQuery: fs.readFileSync(javascriptInjections, 'utf8'),
    },
  })
  const bridge = createBridge(
    session,
    buffer,
    root,
    new Map([['regex', regex]]),
  )
  return {session, buffer, bridge}
}

test('reuses query child backends while public layer identities stay revision-scoped', async () => {
  const fixture = queryFixture('const marker = 0;\nconst first = /a+/;\n')
  const {session, buffer, bridge} = fixture
  await apply(session, buffer, 1)
  const initial = await bridge.synchronize()
  assert.equal(initial.accepted, true)
  const initialDiagnostics = session.getDiagnostics()
  assert.equal(initialDiagnostics.parsedInjectionLayerCount, 1)
  assert.equal(initialDiagnostics.injectionChildBackendCount, 1)
  const initialQuery = queryHighlights(session)
  const initialRegex = unpack(initialQuery).find(
    (capture) => capture.grammarId === 'source.regexp',
  )
  assert.ok(initialRegex)

  buffer.setTextInRange(
    {start: {row: 0, column: 15}, end: {row: 0, column: 16}},
    '1',
  )
  await apply(session, buffer, 2, new Uint32Array([0, 15, 0, 16, 0, 15, 0, 16]))
  const outside = await bridge.synchronize()
  assert.equal(outside.reusedLayers, 1)
  assert.equal(outside.projectedRanges, 1)
  assert.equal(outside.childIncrementalParses, 1)
  assert.equal(outside.childFullParses, 0)
  const outsideQuery = queryHighlights(session)
  const outsideRegex = unpack(outsideQuery).find(
    (capture) => capture.grammarId === 'source.regexp',
  )
  assert.ok(outsideRegex)
  assert.notEqual(outsideRegex.layerId, initialRegex.layerId)
  assert.throws(
    () =>
      session.resolveQueryNode(
        {
          layerId: initialRegex.layerId,
          nodeHandle: initialRegex.nodeHandle,
        },
        revisionTags(session),
      ),
    (error) => error.code === 'ERR_STALE_INJECTION_NODE',
  )

  buffer.setTextInRange(
    {start: {row: 1, column: 15}, end: {row: 1, column: 16}},
    'c',
  )
  await apply(session, buffer, 3, new Uint32Array([1, 15, 1, 16, 1, 15, 1, 16]))
  const inside = await bridge.synchronize()
  assert.equal(inside.reusedLayers, 1)
  assert.equal(inside.childIncrementalParses, 1)
  assert.equal(inside.childFullParses, 0)

  buffer.setTextInRange(
    {start: {row: 0, column: 0}, end: {row: 0, column: 0}},
    '\n',
  )
  await apply(session, buffer, 4, new Uint32Array([0, 0, 0, 0, 0, 0, 1, 0]))
  const shifted = await bridge.synchronize()
  assert.equal(shifted.reusedLayers, 1)
  assert.equal(shifted.projectedRanges, 1)
  assert.equal(shifted.childIncrementalParses, 1)
  assert.equal(shifted.childFullParses, 0)

  const oracle = queryFixture(buffer.getText())
  await apply(oracle.session, oracle.buffer, 1)
  await oracle.bridge.synchronize()
  assert.deepEqual(
    semanticCaptures(queryHighlights(session)),
    semanticCaptures(queryHighlights(oracle.session)),
  )

  oracle.bridge.destroy()
  await oracle.session.destroy()
  bridge.destroy()
  await session.destroy()
})

test('reuses unaffected dynamic siblings and prunes retired child backends', async () => {
  const source =
    '<script>const first = 1;</script>\n' +
    '<script>const second = 2;</script>\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  const html = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html'),
  )
  const javascript = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript'),
    ['javascript'],
  )
  html.addInjectionPoint({
    type: 'script_element',
    language: () => 'javascript',
    content: (node) => node.child(1),
  })
  session.setLanguage(html.descriptor)
  const bridge = createBridge(
    session,
    buffer,
    html,
    new Map([['javascript', javascript]]),
  )
  await apply(session, buffer, 1)
  await bridge.synchronize()
  assert.equal(session.getDiagnostics().injectionChildBackendCount, 2)

  buffer.setTextInRange(
    {start: {row: 0, column: 22}, end: {row: 0, column: 23}},
    '100',
  )
  await apply(session, buffer, 2, new Uint32Array([0, 22, 0, 23, 0, 22, 0, 25]))
  const changed = await bridge.synchronize()
  assert.equal(changed.reusedLayers, 1)
  assert.equal(changed.childIncrementalParses, 1)
  assert.equal(changed.childFullParses, 1)
  assert.equal(changed.reuseFallback, true)
  assert.equal(session.getDiagnostics().injectionChildBackendCount, 2)

  let revision = 2
  for (let iteration = 0; iteration < 10; iteration++) {
    const current = iteration % 2 === 0 ? '2' : '3'
    const marker = buffer.getText().indexOf(`${current};</script>`)
    const start = buffer.positionForCharacterIndex(marker)
    const replacement = current === '2' ? '3' : '2'
    buffer.setTextInRange(
      {start, end: {row: start.row, column: start.column + 1}},
      replacement,
    )
    await apply(
      session,
      buffer,
      ++revision,
      new Uint32Array([
        start.row,
        start.column,
        start.row,
        start.column + 1,
        start.row,
        start.column,
        start.row,
        start.column + 1,
      ]),
    )
    const reused = await bridge.synchronize()
    assert.equal(reused.reusedLayers, 2)
    assert.equal(reused.childIncrementalParses, 2)
    assert.equal(reused.childFullParses, 0)
    assert.equal(session.getDiagnostics().injectionChildBackendCount, 2)
  }

  buffer.setText('const noLongerHtml = true;\n')
  await apply(session, buffer, revision + 1)
  await bridge.synchronize()
  assert.equal(session.getDiagnostics().injectionLayerCount, 0)
  assert.equal(session.getDiagnostics().injectionChildBackendCount, 0)

  bridge.destroy()
  await session.destroy()
})

test('bounds overlapping same-revision synchronizations and publishes the latest child tree', async () => {
  const source = [
    'const marker = 0;',
    ...Array.from(
      {length: 300},
      (_, index) => `const pattern_${index} = /^value_${index}+$/;`,
    ),
  ].join('\n')
  const {session, buffer, bridge} = queryFixture(source)
  await apply(session, buffer, 1)
  const [superseded, latest] = await Promise.allSettled([
    bridge.synchronize(),
    bridge.synchronize(),
  ])
  assert.equal(superseded.status, 'fulfilled')
  assert.equal(latest.status, 'fulfilled')
  assert.equal(superseded.value.accepted, false)
  assert.equal(latest.value.accepted, true)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.activeInjectionJobs, 0)
  assert.equal(diagnostics.injectionActiveRequests, 0)
  assert.equal(diagnostics.injectionChildBackendCount, 1)

  const oracle = queryFixture(source)
  await apply(oracle.session, oracle.buffer, 1)
  await oracle.bridge.synchronize()
  assert.deepEqual(
    semanticCaptures(queryHighlights(session, 400)),
    semanticCaptures(queryHighlights(oracle.session, 400)),
  )

  oracle.bridge.destroy()
  await oracle.session.destroy()
  bridge.destroy()
  await session.destroy()
})

test('newer wave zero supersedes an older nested rescan without repair', async () => {
  const commentCount = 100
  const comments = Array.from(
    {length: commentCount},
    (_, index) => `// TODO ${index}`,
  ).join('\n')
  const buffer = new TextBuffer(`<script>\n${comments}\n</script>`)
  const session = new DocumentSession()
  const html = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html'),
  )
  const javascript = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript'),
    ['javascript'],
  )
  const todo = new FakeGrammar(
    'text.todo',
    descriptor('text.todo', todoWasm, 'TODO'),
    ['todo'],
  )
  html.addInjectionPoint({
    type: 'script_element',
    language: () => 'javascript',
    content: (node) => node.child(1),
  })
  javascript.addInjectionPoint({
    type: 'comment',
    language: (node) => (node.text.includes('TODO') ? 'todo' : null),
    content: (node) => node,
  })
  session.setLanguage(html.descriptor)
  await apply(session, buffer, 1)

  let bridge
  let latest = null
  let launched = false
  let markLatestStarted
  const latestStarted = new Promise((resolve) => {
    markLatestStarted = resolve
  })
  const engine = new Proxy(session, {
    get(target, property) {
      const value = target[property]
      if (property === 'getInjectionCandidates') {
        return (request) => {
          const pending = value.call(target, request)
          if (request.injectionRescanWave === 1 && !launched) {
            launched = true
            setImmediate(() => {
              latest = bridge.synchronize()
              markLatestStarted()
            })
          }
          return pending
        }
      }
      return typeof value === 'function' ? value.bind(target) : value
    },
  })
  bridge = new DynamicInjectionBridge({
    engine,
    buffer,
    grammar: html,
    resolveGrammar(name) {
      if (name === 'javascript') return javascript
      if (name === 'todo') return todo
      return null
    },
    getCurrentTags: () => revisionTags(session),
    batchSize: 64,
  })

  try {
    const superseded = bridge.synchronize()
    let timeout
    await Promise.race([
      latestStarted,
      new Promise((_, reject) => {
        timeout = setTimeout(
          () => reject(new Error('Nested rescan wave did not start')),
          10_000,
        )
      }),
    ])
    clearTimeout(timeout)
    const [oldResult, latestResult] = await Promise.all([superseded, latest])

    assert.equal(oldResult.accepted, false)
    assert.equal(oldResult.reason, 'stale-candidates')
    assert.equal(latestResult.accepted, true)
    const diagnostics = session.getDiagnostics()
    assert.equal(diagnostics.maximumInjectionDepth, 2)
    assert.equal(diagnostics.injectionLayerCount, commentCount + 1)
    assert.equal(diagnostics.injectionActiveRequests, 0)
    assert.equal(diagnostics.activeInjectionJobs, 0)
  } finally {
    bridge.destroy()
    await session.destroy()
  }
})
