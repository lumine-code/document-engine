'use strict'

const assert = require('node:assert/strict')
const path = require('node:path')
const test = require('node:test')
const {DocumentSession} = require('../..')
const {loadSuperstring} = require('../helpers')
const {
  DynamicInjectionBridge,
  decodeCandidates,
} = require('../../../lumine/src/dynamic-injection-bridge')

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
const htmlWasm = path.resolve(
  __dirname,
  '..',
  '..',
  '..',
  'language-html',
  'grammars',
  'html.wasm',
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

function descriptor(languageId, wasmPath, languageName) {
  return {
    languageId,
    runtime: 'wasm',
    wasmPath,
    languageName,
    languageSegment: languageName,
    queryPaths: {},
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

function revisionTags(session) {
  const diagnostics = session.getDiagnostics()
  return {
    bufferRevision: diagnostics.bufferRevision,
    syntaxRevision: diagnostics.syntaxRevision,
    languageGeneration: diagnostics.languageGeneration,
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

  removeInjectionPoint(point) {
    const points = this.injectionPointsByType[point.type] ?? []
    const index = points.indexOf(point)
    if (index !== -1) points.splice(index, 1)
    if (points.length === 0) delete this.injectionPointsByType[point.type]
    for (const listener of this.removeListeners) listener(point)
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

test('runs real JS injection callbacks against revision-scoped native node facades', async () => {
  const buffer = new TextBuffer('html`<main>${name}</main>`\n')
  const session = new DocumentSession()
  const root = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript'),
  )
  const target = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html'),
    ['html'],
  )
  let callbackNode
  root.addInjectionPoint({
    type: 'call_expression',
    language(node) {
      callbackNode = node
      assert.equal(node.type, 'call_expression')
      assert.equal(node.firstChild.text, 'html')
      assert.equal(node.firstChild.parent.id, node.id)
      assert.equal(node.children.length, node.childCount)
      assert.equal(node.namedChildren.length, node.namedChildCount)
      assert.equal(node.child(0).id, node.firstChild.id)
      assert.equal(node.lastNamedChild.type, 'template_string')
      assert.ok(node.range.end.column > node.range.start.column)
      return 'html'
    },
    content(node) {
      return node.lastChild.children.filter(
        (child) => child.type === 'string_fragment',
      )
    },
  })
  session.setLanguage(root.descriptor)
  await apply(session, buffer, 1)
  const bridge = new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar: root,
    resolveGrammar: (name) => (name === 'html' ? target : null),
    getCurrentTags: () => revisionTags(session),
  })

  const result = await bridge.synchronize()
  assert.equal(result.accepted, true)
  assert.ok(callbackNode)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.dynamicInjectionLayerCount, 1)
  assert.equal(diagnostics.injectionLayerCount, 1)
  assert.ok(diagnostics.injectionRangeCount >= 1)

  buffer.setText('html`<aside>${name}</aside>`\n')
  await apply(session, buffer, 2)
  assert.equal(callbackNode.type, 'call_expression')
  assert.equal(callbackNode.text, 'html`<main>${name}</main>`')
  assert.equal(callbackNode.isCurrent(), false)
  assert.throws(
    () => callbackNode.assertCurrent(),
    (error) => error.code === 'ERR_STALE_INJECTION_NODE',
  )

  bridge.destroy()
  await session.destroy()
})

test('clips PHP-style Infinity specs and completes the dynamic language-scope phase', async () => {
  const buffer = new TextBuffer('const answer = 42\n')
  const callbackBuffer = {getPath: () => path.resolve('fixture.php')}
  const session = new DocumentSession()
  const root = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript'),
  )
  const target = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html'),
    ['html'],
  )
  root.addInjectionPoint({
    type: 'program',
    language: () => 'html',
    content(node, callbackBuffer) {
      assert.match(callbackBuffer.getPath(), /fixture\.php$/)
      assert.equal(node.descendantsOfType('number').length, 1)
      return {
        startIndex: node.startIndex,
        startPosition: node.startPosition,
        endIndex: Infinity,
        endPosition: {row: Infinity, column: Infinity},
      }
    },
    includeChildren: true,
    languageScope(grammar, _buffer, range) {
      return [
        grammar.scopeName,
        range.start.row === range.end.row
          ? 'meta.embedded.line'
          : 'meta.embedded.block',
      ]
    },
  })
  session.setLanguage(root.descriptor)
  await apply(session, buffer, 1)
  const bridge = new DynamicInjectionBridge({
    engine: session,
    buffer: callbackBuffer,
    grammar: root,
    resolveGrammar: (name) => (name === 'html' ? target : null),
    getCurrentTags: () => revisionTags(session),
  })

  const result = await bridge.synchronize()
  assert.equal(result.accepted, true)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.injectionActiveRequests, 0)
  assert.equal(diagnostics.dynamicInjectionLayerCount, 1)
  assert.equal(diagnostics.injectionRangeCount, 1)

  bridge.destroy()
  await session.destroy()
})

test('cancels candidate scans when the syntax revision is replaced or destroyed', async () => {
  const buffer = new TextBuffer('/* comment */\n'.repeat(20000))
  const session = new DocumentSession()
  session.setLanguage(descriptor('source.js', javascriptWasm, 'javascript'))
  await apply(session, buffer, 1)
  const tags = revisionTags(session)
  const candidates = session.getInjectionCandidates({
    ...tags,
    injectionPointGeneration: 1,
    grammars: [{grammarId: 'source.js', types: ['comment'], registrations: []}],
  })
  const destroying = session.destroy()
  const result = await candidates
  assert.equal(result.accepted, false)
  assert.equal(result.reason, 'stale-candidates')
  await destroying
})

test('packs native candidates with resolvable handles', async () => {
  const buffer = new TextBuffer('const first = 1\nconst second = 2\n')
  const session = new DocumentSession()
  session.setLanguage(descriptor('source.js', javascriptWasm, 'javascript'))
  await apply(session, buffer, 1)
  const tags = revisionTags(session)
  const batch = await session.getInjectionCandidates({
    ...tags,
    injectionPointGeneration: 1,
    grammars: [
      {
        grammarId: 'source.js',
        types: ['lexical_declaration'],
        registrations: [],
      },
    ],
  })
  assert.equal(batch.queryLayerCount, 0)
  for (const field of [
    'candidateQueueMilliseconds',
    'candidateScanMilliseconds',
    'candidatePackMilliseconds',
  ]) {
    assert.ok(Number.isFinite(batch[field]))
    assert.ok(batch[field] >= 0)
  }
  const candidates = decodeCandidates(batch)
  assert.equal(candidates.length, 2)
  assert.deepEqual(
    candidates.map(
      (candidate) => session.resolveInjectionNode(candidate, tags).text,
    ),
    ['const first = 1', 'const second = 2'],
  )
  session.abortInjectionRequest({
    ...tags,
    requestId: batch.requestId,
    reason: 'test-complete',
  })
  await session.destroy()
})

test('deduplicates handle caches when candidate scans race', async () => {
  const declarationCount = 2000
  const buffer = new TextBuffer(
    Array.from(
      {length: declarationCount},
      (_, index) => `const value_${index} = ${index}`,
    ).join('\n'),
  )
  const session = new DocumentSession()
  session.setLanguage(descriptor('source.js', javascriptWasm, 'javascript'))
  await apply(session, buffer, 1)
  const tags = revisionTags(session)
  const request = {
    ...tags,
    injectionPointGeneration: 1,
    grammars: [
      {
        grammarId: 'source.js',
        types: ['lexical_declaration'],
        registrations: [],
      },
    ],
  }

  const batches = await Promise.all(
    Array.from({length: 4}, () => session.getInjectionCandidates(request)),
  )
  for (const batch of batches) {
    assert.equal(decodeCandidates(batch).length, declarationCount)
    session.abortInjectionRequest({
      ...tags,
      requestId: batch.requestId,
      reason: 'test-complete',
    })
  }

  const cached = await session.getInjectionCandidates(request)
  assert.equal(decodeCandidates(cached).length, declarationCount)
  session.abortInjectionRequest({
    ...tags,
    requestId: cached.requestId,
    reason: 'test-complete',
  })
  await session.destroy()
})

test('reports injection topology changes without invalidating stable empty results', async () => {
  const buffer = new TextBuffer('alpha\nbeta\n')
  const session = new DocumentSession()
  const root = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript'),
  )
  const html = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html'),
    ['html'],
  )
  const todo = new FakeGrammar(
    'text.todo',
    descriptor('text.todo', todoWasm, 'TODO'),
    ['todo'],
  )
  let languageName = 'html'
  let contentRange = {
    startIndex: 0,
    endIndex: 5,
    startPosition: {row: 0, column: 0},
    endPosition: {row: 0, column: 5},
  }
  const injectionPoint = {
    type: 'program',
    language: () => languageName,
    content: () => contentRange,
    includeChildren: true,
  }
  session.setLanguage(root.descriptor)
  await apply(session, buffer, 1)
  const bridge = new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar: root,
    resolveGrammar(name) {
      if (name === 'html') return html
      if (name === 'todo') return todo
      return null
    },
    getCurrentTags: () => revisionTags(session),
  })

  const emptyResult = await bridge.synchronize()
  assert.equal(emptyResult.topologyChanged, false)
  root.addInjectionPoint(injectionPoint)
  assert.equal((await bridge.synchronize()).topologyChanged, true)
  assert.equal((await bridge.synchronize()).topologyChanged, true)

  contentRange = {
    startIndex: 6,
    endIndex: 10,
    startPosition: {row: 1, column: 0},
    endPosition: {row: 1, column: 4},
  }
  assert.equal((await bridge.synchronize()).topologyChanged, true)

  languageName = 'todo'
  assert.equal((await bridge.synchronize()).topologyChanged, true)

  root.removeInjectionPoint(injectionPoint)
  assert.equal((await bridge.synchronize()).topologyChanged, true)
  assert.equal((await bridge.synchronize()).topologyChanged, false)

  bridge.destroy()
  await session.destroy()
})

test('publishes query-defined injection ranges without a JS callback round trip', async () => {
  const buffer = new TextBuffer('// native query injection\nconst value = 1\n')
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {
      injections: `
        ((comment) @injection.content
          (#set! injection.language "comment"))
      `,
    },
  })
  await apply(session, buffer, 1)

  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.queryInjectionLayerCount, 1)
  assert.equal(diagnostics.dynamicInjectionLayerCount, 0)
  assert.equal(diagnostics.injectionLayerCount, 1)
  assert.equal(diagnostics.injectionRangeCount, 1)
  await session.destroy()
})

test('parses HTML to JavaScript to TODO as nested native child layers', async () => {
  const buffer = new TextBuffer(
    '<script>const value = 1 // TODO nested\n</script>\n',
  )
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
  const bridge = new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar: html,
    resolveGrammar(name) {
      if (name === 'javascript') return javascript
      if (name === 'todo') return todo
      return null
    },
    getCurrentTags: () => revisionTags(session),
  })

  const result = await bridge.synchronize()
  assert.equal(result.accepted, true)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.dynamicInjectionLayerCount, 2)
  assert.equal(diagnostics.parsedInjectionLayerCount, 2)
  assert.equal(diagnostics.failedInjectionLayerCount, 0)
  assert.equal(diagnostics.maximumInjectionDepth, 2)

  bridge.destroy()
  await session.destroy()
})
