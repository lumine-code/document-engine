'use strict'

const assert = require('node:assert/strict')
const path = require('node:path')
const test = require('node:test')
const {DocumentSession, capabilities} = require('../..')
const {loadSuperstring} = require('../helpers')
const {
  DynamicInjectionBridge,
} = require('../../../lumine/src/dynamic-injection-bridge')

const {TextBuffer} = loadSuperstring()
const workspaceRoot = path.resolve(__dirname, '..', '..', '..')
const ipythonWasm = path.join(
  workspaceRoot,
  'language-ipython',
  'grammars',
  'ipython.wasm',
)
const htmlWasm = path.join(
  workspaceRoot,
  'language-html',
  'grammars',
  'html.wasm',
)
const javascriptWasm = path.join(
  workspaceRoot,
  'language-javascript',
  'grammars',
  'javascript.wasm',
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

test('walks IPython parents, fields, children, and siblings without materializing a tree', async () => {
  assert.equal(capabilities.nativeSyntaxNodeApi, true)
  const source = 'def answer(value):\n    return value\n%time answer(1)\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.setLanguage(descriptor('source.python.ipy', ipythonWasm, 'ipython'))
  await apply(session, buffer, 1)

  const found = session.getSyntaxNodeAtPosition({row: 0, column: 5})
  assert.equal(found.current, true)
  assert.equal(found.grammarId, 'source.python.ipy')
  assert.equal(found.depth, 0)
  assert.equal(found.layerId, 0)
  assert.ok(found.candidates.length >= 1)
  assert.equal(found.node.type, 'identifier')
  assert.equal(found.node.text, 'answer')
  assert.equal(found.node.isNamed, true)
  assert.equal(found.node.isMissing, false)
  assert.equal(found.node.isError, false)
  assert.equal(found.node.isCurrent(), true)
  assert.equal(found.node.assertCurrent(), found.node)

  const parentTypes = []
  let functionNode = null
  for (let node = found.node; node; node = node.parent) {
    parentTypes.push(node.type)
    if (node.type === 'function_definition') functionNode = node
  }
  assert.ok(parentTypes.includes('function_definition'))
  assert.ok(parentTypes.includes('module'))
  assert.ok(functionNode)
  assert.equal(functionNode.childForFieldName('name').text, 'answer')
  assert.equal(functionNode.childForFieldName('parameters').type, 'parameters')
  assert.ok(functionNode.children.length >= functionNode.namedChildren.length)
  assert.equal(functionNode.childCount, functionNode.children.length)
  assert.equal(functionNode.namedChildCount, functionNode.namedChildren.length)
  assert.equal(functionNode.firstChild.id, functionNode.child(0).id)
  assert.equal(functionNode.firstNamedChild.id, functionNode.namedChild(0).id)
  assert.ok(
    functionNode.descendantsOfType(['identifier', 'return_statement']).length >=
      3,
  )

  const parameter = functionNode.descendantsOfType('identifier')[1]
  assert.equal(parameter.text, 'value')
  assert.ok(parameter.previousSibling || parameter.nextSibling)
  assert.equal(
    functionNode.childForFieldName('name').nextNamedSibling.type,
    'parameters',
  )

  const containing = session.getSyntaxNodeContainingRange({
    start: {row: 0, column: 4},
    end: {row: 0, column: 10},
  })
  assert.equal(containing.current, true)
  assert.equal(containing.node.type, 'function_definition')
  await session.destroy()
})

test('selects the deepest injected syntax layer and exposes every tied layer candidate', async () => {
  const source = '<script>const injected = 1;</script>\n'
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
  await apply(session, buffer, 1)
  const bridge = new DynamicInjectionBridge({
    engine: session,
    buffer,
    grammar: html,
    resolveGrammar: (name) => (name === 'javascript' ? javascript : null),
    getCurrentTags: () => revisionTags(session),
  })
  assert.equal((await bridge.synchronize()).accepted, true)

  const found = session.getSyntaxNodeAtPosition({row: 0, column: 14})
  assert.equal(found.current, true)
  assert.equal(found.grammarId, 'source.js')
  assert.equal(found.depth, 1)
  assert.ok(found.layerId > 0)
  assert.equal(found.node.type, 'identifier')
  assert.equal(found.node.text, 'injected')
  assert.ok(
    found.candidates.some((candidate) => candidate.grammarId === 'source.js'),
  )
  assert.ok(
    found.candidates.some(
      (candidate) => candidate.grammarId === 'text.html.basic',
    ),
  )

  bridge.destroy()
  await session.destroy()
})

test('keeps stale handles readable while current-only operations reject after edit and close', async () => {
  const buffer = new TextBuffer('def answer():\n    return 1\n')
  const session = new DocumentSession()
  session.setLanguage(descriptor('source.python.ipy', ipythonWasm, 'ipython'))
  await apply(session, buffer, 1)
  const oldResult = session.getSyntaxNodeAtPosition({row: 0, column: 5})
  const oldNode = oldResult.node
  assert.equal(oldNode.text, 'answer')

  buffer.setText('def changed():\n    return 1\n')
  const snapshot = buffer.getSnapshot()
  const pending = session.applyRevision(snapshot, new Uint32Array(0), 2)
  snapshot.destroy()
  const duringParse = session.getSyntaxNodeAtPosition({row: 0, column: 5})
  assert.equal(duringParse.current, false)
  assert.equal(duringParse.node, null)
  await pending
  assert.equal(oldNode.text, 'answer')
  assert.equal(oldNode.type, 'identifier')
  assert.equal(oldNode.parent.type, 'function_definition')
  assert.equal(oldNode.isCurrent(), false)
  assert.throws(
    () => oldNode.assertCurrent(),
    (error) => error.code === 'ERR_STALE_INJECTION_NODE',
  )
  const current = session.getSyntaxNodeAtPosition({row: 0, column: 5})
  assert.equal(current.node.text, 'changed')
  assert.equal(current.node.isCurrent(), true)

  await session.destroy()
  assert.equal(oldNode.text, 'answer')
  assert.equal(oldNode.parent.childForFieldName('name').text, 'answer')
  assert.equal(oldNode.isCurrent(), false)
})

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
