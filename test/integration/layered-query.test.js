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
const javascriptHighlights = grammarPath(
  'language-javascript',
  'javascript-highlights.scm',
)
const htmlWasm = grammarPath('language-html', 'html.wasm')
const htmlHighlights = grammarPath('language-html', 'html-highlights.scm')
const todoWasm = grammarPath('language-todo', 'todo.wasm')
const todoHighlights = grammarPath('language-todo', 'todo-highlights.scm')

function descriptor(languageId, wasmPath, languageName, highlightsPath) {
  return {
    languageId,
    runtime: 'wasm',
    wasmPath,
    languageName,
    languageSegment: languageName,
    queryPaths: highlightsPath ? {highlightsQuery: [highlightsPath]} : {},
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

function beginApply(session, buffer, revision, edits) {
  const snapshot = buffer.getSnapshot()
  const promise = session.applyRevision(snapshot, edits, revision)
  snapshot.destroy()
  return promise
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
      layerIndex,
      layerId: result.layers[layerIndex].layerId,
      grammarId: result.layers[layerIndex].grammarId,
      depth: result.captureDepths[captureIndex],
      order: result.captureOrders[captureIndex],
      flags: result.captureFlags[captureIndex],
      nodeHandle: result.captureNodeHandles[captureIndex],
    })
  }
  return captures
}

function layerRanges(layer) {
  const result = []
  for (
    let offset = 0;
    offset < layer.ranges.length;
    offset += layer.rangeStride
  ) {
    result.push({
      startIndex: layer.ranges[offset + 4],
      endIndex: layer.ranges[offset + 5],
    })
  }
  return result
}

function overlaps(capture, range) {
  return (
    capture.startIndex < range.endIndex && range.startIndex < capture.endIndex
  )
}

function createNestedGrammars({coverTodo = true} = {}) {
  const html = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html', htmlHighlights),
  )
  const javascript = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript', javascriptHighlights),
    ['javascript'],
  )
  const todo = new FakeGrammar(
    'text.todo',
    descriptor('text.todo', todoWasm, 'TODO', todoHighlights),
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
    coverShallowerScopes: coverTodo,
  })
  return {html, javascript, todo}
}

function createBridge(session, buffer, grammars) {
  return new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar: grammars.html,
    resolveGrammar(name) {
      if (name === 'javascript') return grammars.javascript
      if (name === 'todo') return grammars.todo
      return null
    },
    getCurrentTags: () => revisionTags(session),
  })
}

test('merges HTML to JavaScript to TODO highlights with stable layer identities', async () => {
  const source = '<script>const value = 1 // TODO nested\n</script>\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  const grammars = createNestedGrammars()
  session.setLanguage(grammars.html.descriptor)
  await apply(session, buffer, 1)
  const bridge = createBridge(session, buffer, grammars)
  assert.equal((await bridge.synchronize()).accepted, true)

  const options = {
    resolveScopes: true,
    interpolateNames: true,
    includeLanguageScopes: true,
  }
  const first = session.getQueryCaptures('highlightsQuery', 0, 10, options)
  const second = session.getQueryCaptures('highlightsQuery', 0, 10, options)
  assert.deepEqual(Array.from(second.captures), Array.from(first.captures))
  assert.deepEqual(
    Array.from(second.captureLayerIndices),
    Array.from(first.captureLayerIndices),
  )
  assert.deepEqual(
    first.layers.map((layer) => layer.depth),
    [0, 1, 2],
  )
  assert.deepEqual(
    first.layers.map((layer) => layer.grammarId),
    ['text.html.basic', 'source.js', 'text.todo'],
  )

  const captures = unpack(first)
  assert.ok(
    captures.some(
      (capture) => capture.depth === 0 && capture.name.endsWith('.html'),
    ),
  )
  assert.ok(
    captures.some(
      (capture) => capture.depth === 1 && capture.name.endsWith('.js'),
    ),
  )
  assert.ok(
    captures.some(
      (capture) =>
        capture.depth === 2 && capture.name === 'storage.type.class.todo',
    ),
  )
  for (const scope of ['text.html.basic', 'source.js', 'text.todo']) {
    assert.ok(
      captures.some(
        (capture) => capture.name === scope && (capture.flags & 1) !== 0,
      ),
      scope,
    )
  }
  for (const capture of captures.filter((capture) => capture.depth > 0)) {
    assert.ok(
      layerRanges(first.layers[capture.layerIndex]).some(
        (range) =>
          capture.startIndex >= range.startIndex &&
          capture.endIndex <= range.endIndex,
      ),
      `${capture.name} is clipped to its included range`,
    )
  }

  const todoCapture = captures.find(
    (capture) =>
      capture.name === 'storage.type.class.todo' && capture.nodeHandle > 0,
  )
  assert.ok(todoCapture)
  const node = session.resolveQueryNode(
    {layerId: todoCapture.layerId, nodeHandle: todoCapture.nodeHandle},
    revisionTags(session),
  )
  assert.match(node.text, /TODO/)

  const todoLayer = first.layers.find((layer) => layer.depth === 2)
  const todoRange = layerRanges(todoLayer)[0]
  assert.ok(
    captures.some(
      (capture) => capture.depth === 2 && overlaps(capture, todoRange),
    ),
  )
  assert.equal(
    captures.filter(
      (capture) =>
        capture.depth < 2 &&
        (capture.flags & 1) === 0 &&
        overlaps(capture, todoRange),
    ).length,
    0,
    'coverShallowerScopes removes shallower query scopes but keeps language scopes',
  )

  bridge.destroy()
  await session.destroy()
})

test('keeps overlapping same-depth layers while a covering layer hides only shallower scopes', async () => {
  const source = 'html`<b>hello</b>`\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  const javascript = new FakeGrammar(
    'source.js',
    descriptor('source.js', javascriptWasm, 'javascript', javascriptHighlights),
  )
  const html = new FakeGrammar(
    'text.html.basic',
    descriptor('text.html.basic', htmlWasm, 'html', htmlHighlights),
    ['html'],
  )
  const injection = (coverShallowerScopes) => ({
    type: 'call_expression',
    language: () => 'html',
    content: (node) =>
      node.lastChild.children.find((child) => child.type === 'string_fragment'),
    coverShallowerScopes,
  })
  javascript.addInjectionPoint(injection(false))
  javascript.addInjectionPoint(injection(true))
  session.setLanguage(javascript.descriptor)
  await apply(session, buffer, 1)
  const bridge = new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar: javascript,
    resolveGrammar: (name) => (name === 'html' ? html : null),
    getCurrentTags: () => revisionTags(session),
  })
  assert.equal((await bridge.synchronize()).accepted, true)

  const result = session.getQueryCaptures('highlightsQuery', 0, 2, {
    resolveScopes: true,
    interpolateNames: true,
    includeLanguageScopes: true,
  })
  const childLayers = result.layers
    .map((layer, index) => ({...layer, index}))
    .filter((layer) => layer.depth === 1)
  assert.equal(childLayers.length, 2)
  const captures = unpack(result)
  for (const layer of childLayers) {
    assert.ok(
      captures.some(
        (capture) =>
          capture.layerIndex === layer.index && (capture.flags & 1) === 0,
      ),
    )
  }
  const coveredRange = layerRanges(
    childLayers.find((layer) => layer.coverShallowerScopes),
  )[0]
  assert.equal(
    captures.filter(
      (capture) =>
        capture.depth === 0 &&
        (capture.flags & 1) === 0 &&
        overlaps(capture, coveredRange),
    ).length,
    0,
  )

  bridge.destroy()
  await session.destroy()
})

test('projects unchanged captures and drops changed scopes while syntax is pending', async () => {
  const source = '<script>const value = 1 // TODO nested\n</script>\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession({workerDelayMs: 40})
  const grammars = createNestedGrammars({coverTodo: false})
  session.setLanguage(grammars.html.descriptor)
  await apply(session, buffer, 1)
  const bridge = createBridge(session, buffer, grammars)
  assert.equal((await bridge.synchronize()).accepted, true)
  const before = session.getQueryCaptures('highlightsQuery', 0, 10, {
    resolveScopes: true,
    interpolateNames: true,
  })
  const changedStart = source.indexOf('TODO')
  const newSource = source.replace('TODO', 'DONE')
  const beforePoint = {row: 0, column: changedStart}
  const afterPoint = {row: 0, column: changedStart + 4}
  buffer.setText(newSource)
  const pending = beginApply(
    session,
    buffer,
    2,
    new Uint32Array([
      beforePoint.row,
      beforePoint.column,
      beforePoint.row,
      beforePoint.column + 4,
      beforePoint.row,
      beforePoint.column,
      afterPoint.row,
      afterPoint.column,
    ]),
  )

  const projected = session.getQueryCaptures('highlightsQuery', 0, 10, {
    resolveScopes: true,
    interpolateNames: true,
  })
  assert.equal(projected.bufferRevision, 2)
  assert.equal(projected.syntaxRevision, 1)
  assert.ok(projected.staleCaptureCount > 0)
  assert.equal(
    unpack(projected).filter(
      (capture) =>
        capture.startIndex < changedStart + 4 &&
        changedStart < capture.endIndex,
    ).length,
    0,
    'no stale scope intersects the edited text',
  )
  assert.ok(projected.captures.length < before.captures.length)
  const oldNode = unpack(projected).find((capture) => capture.nodeHandle > 0)
  if (oldNode) {
    assert.throws(
      () =>
        session.resolveQueryNode(
          {layerId: oldNode.layerId, nodeHandle: oldNode.nodeHandle},
          revisionTags(session),
        ),
      (error) => error.code === 'ERR_STALE_INJECTION_NODE',
    )
  }

  await pending
  assert.equal((await bridge.synchronize()).accepted, true)
  const current = session.getQueryCaptures('highlightsQuery', 0, 10, {
    resolveScopes: true,
    interpolateNames: true,
  })
  assert.equal(current.syntaxRevision, 2)
  assert.equal(
    unpack(current).some((capture) => capture.depth === 2),
    false,
  )

  bridge.destroy()
  await session.destroy()
})

test('resolves query-defined language aliases before parsing the child layer', async () => {
  const source = '<script>const answer = 42</script>\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'text.html.basic',
    wasmPath: htmlWasm,
    languageName: 'html',
    queries: {
      highlightsQuery: fs.readFileSync(htmlHighlights, 'utf8'),
      injectionsQuery: String.raw`
        (script_element
          (raw_text) @injection.content
          (#set! injection.language "javascript"))
        (script_element
          (raw_text) @injection.content
          (#set! injection.language "missing-language"))
      `,
    },
  })
  await apply(session, buffer, 1)
  const tags = revisionTags(session)
  const candidates = await session.getInjectionCandidates({
    ...tags,
    injectionPointGeneration: 1,
    grammars: [],
  })
  assert.deepEqual(candidates.unresolvedQueryLanguages, [
    'javascript',
    'missing-language',
  ])
  const resolution = session.applyQueryLanguageDescriptors({
    ...tags,
    requestId: candidates.requestId,
    injectionPointGeneration: 1,
    descriptors: [
      {
        alias: 'javascript',
        grammar: descriptor(
          'source.js',
          javascriptWasm,
          'javascript',
          javascriptHighlights,
        ),
      },
      {alias: 'missing-language', grammar: null},
    ],
  })
  assert.equal(resolution.accepted, true)
  assert.deepEqual(resolution.resolvedAliases, ['javascript'])
  assert.deepEqual(resolution.rejectedAliases, ['missing-language'])
  const publication = await session.applyInjectionResultBatch({
    ...tags,
    requestId: candidates.requestId,
    injectionPointGeneration: 1,
    batchIndex: 0,
    isFinal: true,
    results: [],
  })
  assert.equal(publication.accepted, true)
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.queryInjectionLanguageResolutions, 1)
  assert.equal(diagnostics.queryInjectionLanguageRejections, 1)
  assert.equal(diagnostics.unresolvedQueryInjectionLanguages, 0)
  assert.equal(diagnostics.parsedInjectionLayerCount, 1)

  const captures = unpack(
    session.getQueryCaptures('highlightsQuery', 0, 2, {
      resolveScopes: true,
      interpolateNames: true,
      includeLanguageScopes: true,
    }),
  )
  assert.ok(
    captures.some(
      (capture) => capture.grammarId === 'source.js' && capture.depth === 1,
    ),
  )
  assert.ok(
    captures.some(
      (capture) => capture.name === 'source.js' && (capture.flags & 1) !== 0,
    ),
  )

  await apply(session, buffer, 2)
  const nextTags = revisionTags(session)
  const nextCandidates = await session.getInjectionCandidates({
    ...nextTags,
    injectionPointGeneration: 2,
    grammars: [],
  })
  assert.deepEqual(nextCandidates.unresolvedQueryLanguages, [])
  session.abortInjectionRequest({
    ...nextTags,
    requestId: nextCandidates.requestId,
    reason: 'negative-cache-verified',
  })

  await session.destroy()
})
