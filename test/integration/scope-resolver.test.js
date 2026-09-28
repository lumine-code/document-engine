'use strict'

const assert = require('node:assert/strict')
const path = require('node:path')
const test = require('node:test')
const TreeSitter = require('web-tree-sitter')
const {loadSuperstring} = require('../helpers')

const {TextBuffer} = loadSuperstring()
const workspaceRoot = path.resolve(__dirname, '..', '..', '..')
const javascriptWasm = path.join(
  workspaceRoot,
  'language-javascript',
  'grammars',
  'javascript.wasm',
)

const previousLumine = global.lumine
const disposable = {dispose() {}}
global.lumine = {
  config: {
    get() {},
    onDidChangeConfiguration() {
      return disposable
    },
  },
  grammars: {
    onDidAddGrammar() {
      return disposable
    },
    onDidUpdateGrammar() {
      return disposable
    },
  },
  window: {isDevMode: () => false},
}

// This is intentionally the production legacy implementation, not a test
// reimplementation of its rules. web-tree-sitter supplies the same capture
// objects that TreeSitterLanguageMode used to pass to it.
const ScopeResolver = require(
  path.join(workspaceRoot, 'lumine', 'src', 'scope-resolver.js'),
)

test.after(() => {
  ScopeResolver.clearConfigCache()
  if (previousLumine === undefined) {
    delete global.lumine
  } else {
    global.lumine = previousLumine
  }
})

let languagePromise
let DocumentSession
async function loadLanguage() {
  if (!languagePromise) {
    languagePromise = (async () => {
      await TreeSitter.Parser.init()
      return TreeSitter.Language.load(javascriptWasm)
    })()
  }
  return languagePromise
}

function createConfig(values) {
  return {
    get(key) {
      return values[key]
    },
    onDidChangeConfiguration() {
      return disposable
    },
  }
}

function createLegacyBuffer(text) {
  const lineStarts = [0]
  for (let index = 0; index < text.length; index++) {
    if (text.charCodeAt(index) === 10) lineStarts.push(index + 1)
  }

  const lineEnd = (row) => {
    const nextStart = lineStarts[row + 1]
    let end = nextStart === undefined ? text.length : nextStart - 1
    if (end > lineStarts[row] && text.charCodeAt(end - 1) === 13) end--
    return end
  }

  const clipPosition = (position) => {
    const row = Math.max(
      0,
      Math.min(Number(position.row), lineStarts.length - 1),
    )
    const column = Math.max(
      0,
      Math.min(Number(position.column), lineEnd(row) - lineStarts[row]),
    )
    return {row, column}
  }

  return {
    characterIndexForPosition(position) {
      const clipped = clipPosition(position)
      return lineStarts[clipped.row] + clipped.column
    },
    positionForCharacterIndex(rawIndex) {
      const index = Math.max(0, Math.min(Number(rawIndex), text.length))
      let low = 0
      let high = lineStarts.length
      while (low < high) {
        const middle = Math.floor((low + high) / 2)
        if (lineStarts[middle] <= index) low = middle + 1
        else high = middle
      }
      const row = Math.max(0, low - 1)
      return clipPosition({row, column: index - lineStarts[row]})
    },
    clipPosition,
    lineForRow(row) {
      if (row < 0 || row >= lineStarts.length) return undefined
      return text.slice(lineStarts[row], lineEnd(row))
    },
  }
}

function normalizeLegacyCapture(capture, range, interpolateNames) {
  return {
    name: interpolateNames
      ? ScopeResolver.interpolateName(capture.name, capture.node)
      : capture.name,
    patternIndex: capture.patternIndex,
    startRow: range.startPosition.row,
    startColumn: range.startPosition.column,
    endRow: range.endPosition.row,
    endColumn: range.endPosition.column,
    startIndex: range.startIndex,
    endIndex: range.endIndex,
  }
}

async function legacyCaptures({
  source,
  querySource,
  scopeConfig = {},
  injectionDepth = 0,
  interpolateNames = false,
  startPosition,
  endPosition,
}) {
  const language = await loadLanguage()
  const parser = new TreeSitter.Parser()
  parser.setLanguage(language)
  const tree = parser.parse(source)
  const query = new TreeSitter.Query(language, querySource)
  const config = createConfig(scopeConfig)
  const resolver = new ScopeResolver({
    buffer: createLegacyBuffer(source),
    depth: injectionDepth,
    grammar: {scopeName: 'source.js'},
    languageMode: {config},
  })

  try {
    const result = []
    const captures =
      startPosition || endPosition
        ? query.captures(tree.rootNode, {startPosition, endPosition})
        : query.captures(tree.rootNode)
    for (const capture of captures) {
      const range = resolver.store(capture, {boundaries: false})
      if (range) {
        result.push(normalizeLegacyCapture(capture, range, interpolateNames))
      }
    }
    return result
  } finally {
    resolver.destroy()
    ScopeResolver.clearConfigCache()
    query.delete()
    tree.delete()
    parser.delete()
  }
}

function unpackNativeCaptures(result) {
  const captures = []
  for (
    let offset = 0;
    offset < result.captures.length;
    offset += result.captureStride
  ) {
    captures.push({
      name: result.captureNames[result.captures[offset]],
      patternIndex: result.captures[offset + 1],
      startRow: result.captures[offset + 3],
      startColumn: result.captures[offset + 4],
      endRow: result.captures[offset + 5],
      endColumn: result.captures[offset + 6],
      startIndex: result.captures[offset + 7],
      endIndex: result.captures[offset + 8],
    })
  }
  return captures
}

async function nativeCaptures({
  source,
  querySource,
  scopeConfig = {},
  injectionDepth = 0,
  interpolateNames = false,
  startPosition,
  endPosition,
}) {
  if (!DocumentSession) ({DocumentSession} = require('../..'))
  const session = new DocumentSession()
  session.configureSyntax({
    languageId: 'source.js',
    wasmPath: javascriptWasm,
    languageName: 'javascript',
    queries: {highlightsQuery: querySource},
  })
  const buffer = new TextBuffer(source)
  const snapshot = buffer.getSnapshot()
  try {
    await session.applyRevision(snapshot, new Uint32Array(0), 1)
    const queryStart = startPosition ?? {row: 0, column: 0}
    const queryEnd = endPosition ?? {row: 0xffffffff, column: 0xffffffff}
    return unpackNativeCaptures(
      session.getQueryCaptures(
        'highlightsQuery',
        queryStart.row,
        queryEnd.row,
        {
          scopeConfig,
          injectionDepth,
          resolveScopes: true,
          interpolateNames,
          startColumn: queryStart.column,
          endColumn: queryEnd.column,
        },
      ),
    )
  } finally {
    snapshot.destroy()
    await session.destroy()
  }
}

async function assertParity(fixture) {
  const expected = await legacyCaptures(fixture)
  const actual = await nativeCaptures(fixture)
  assert.deepEqual(actual, expected)
  return expected
}

function assertNameCounts(captures, expected) {
  for (const [name, count] of Object.entries(expected)) {
    assert.equal(
      captures.filter((capture) => capture.name === name).length,
      count,
      name,
    )
  }
}

function capturedText(source, captures, name) {
  const capture = captures.find((candidate) => candidate.name === name)
  assert.ok(capture, name)
  return source.slice(capture.startIndex, capture.endIndex)
}

test('legacy oracle resolves query captures through the production ScopeResolver', async () => {
  const captures = await legacyCaptures({
    source: 'const value = true;\n',
    querySource: String.raw`
      ((identifier) @kept
        (#eq? @kept "value")
        (#is? vendor.unknown "ignored"))
    `,
  })
  assert.deepEqual(
    captures.map((capture) => capture.name),
    ['kept'],
  )
  assert.equal(capturedText('const value = true;\n', captures, 'kept'), 'value')
})

test('matches legacy range adjustments in UTF-16 and interpolates capture names', async () => {
  const source = 'const value = {alpha: "😀TODO:end"};\n'
  const querySource = String.raw`
    ((object) @object.interior
      (#set! adjust.startAt firstChild.endPosition)
      (#set! adjust.endAt lastChild.startPosition))
    ((string) @string.offset
      (#set! adjust.offsetStart 1)
      (#set! adjust.offsetEnd -1))
    ((string) @regex.around
      (#set! adjust.startAndEndAroundFirstMatchOf "😀TODO"))
    ((string) @regex.start.before
      (#set! adjust.startBeforeFirstMatchOf "😀"))
    ((string) @regex.start.after
      (#set! adjust.startAfterFirstMatchOf "😀"))
    ((string) @regex.end.before
      (#set! adjust.endBeforeFirstMatchOf "end"))
    ((string) @regex.end.after
      (#set! adjust.endAfterFirstMatchOf "end"))
    ((string) @regex.lookbehind
      (#set! adjust.startAndEndAroundFirstMatchOf "(?<=😀)TODO"))
    ((string) @regex.invalid
      (#set! adjust.startAndEndAroundFirstMatchOf "["))
  `
  const captures = await assertParity({source, querySource})

  assert.equal(
    capturedText(source, captures, 'object.interior'),
    'alpha: "😀TODO:end"',
  )
  assert.equal(capturedText(source, captures, 'string.offset'), '😀TODO:end')
  assert.equal(capturedText(source, captures, 'regex.around'), '😀TODO')
  assert.equal(
    capturedText(source, captures, 'regex.start.before'),
    '😀TODO:end"',
  )
  assert.equal(capturedText(source, captures, 'regex.start.after'), 'TODO:end"')
  assert.equal(capturedText(source, captures, 'regex.end.before'), '"😀TODO:')
  assert.equal(capturedText(source, captures, 'regex.end.after'), '"😀TODO:end')
  assert.equal(capturedText(source, captures, 'regex.lookbehind'), 'TODO')
  assertNameCounts(captures, {'regex.invalid': 0})

  const interpolated = await assertParity({
    source,
    querySource: '"const" @keyword._TYPE_._TEXT_',
    interpolateNames: true,
  })
  assert.deepEqual(
    interpolated.map((capture) => capture.name),
    ['keyword.const.const'],
  )
})

test('matches final, shy, _IGNORE_, and exact-range overwrite semantics', async () => {
  const claims = await assertParity({
    source: 'locked used fresh bare\n',
    querySource: String.raw`
      ((identifier) @before.final (#eq? @before.final "locked"))
      ((identifier) @claim.final
        (#eq? @claim.final "locked")
        (#set! capture.final))
      ((identifier) @after.final (#eq? @after.final "locked"))
      ((identifier) @before.shy (#eq? @before.shy "used"))
      ((identifier) @after.shy
        (#eq? @after.shy "used")
        (#set! capture.shy))
      ((identifier) @first.shy
        (#eq? @first.shy "fresh")
        (#set! capture.shy false))
      ((identifier) @bare.before (#eq? @bare.before "bare"))
      ((identifier) @bare.final
        (#eq? @bare.final "bare")
        (#set! final false))
      ((identifier) @bare.after (#eq? @bare.after "bare"))
    `,
  })
  assertNameCounts(claims, {
    'before.final': 1,
    'claim.final': 1,
    'after.final': 0,
    'before.shy': 1,
    'after.shy': 0,
    'first.shy': 1,
    'bare.before': 1,
    'bare.final': 1,
    'bare.after': 1,
  })

  const overwritten = await assertParity({
    source: 'const answer = true;\n',
    querySource: String.raw`
      ((true) @_IGNORE_.old (#set! old "value"))
      ((true) @_IGNORE_.replacement (#set! replacement "yes"))
      ((true) @range.after-overwrite
        (#is? test.rangeWithData "replacement yes")
        (#is-not? test.rangeWithData old))
    `,
  })
  assert.deepEqual(
    overwritten.map((capture) => capture.name),
    ['range.after-overwrite'],
  )
})

test('matches legacy stateful resolution for partial-column query ranges', async () => {
  const source = 'a; b;'
  const range = {
    startPosition: {row: 0, column: 3},
    endPosition: {row: 0, column: 4},
  }
  const moveToNextIdentifier = String.raw`
    (#set! adjust.startAt parent.nextNamedSibling.firstNamedChild.startPosition)
    (#set! adjust.endAt parent.nextNamedSibling.firstNamedChild.endPosition)
  `

  const adjustedOutside = await assertParity({
    source,
    ...range,
    querySource: String.raw`
      ((identifier) @inside.adjusted-outside
        (#eq? @inside.adjusted-outside "b")
        (#set! adjust.startAt parent.previousNamedSibling.firstNamedChild.startPosition)
        (#set! adjust.endAt parent.previousNamedSibling.firstNamedChild.endPosition))
    `,
  })
  assert.deepEqual(
    adjustedOutside.map(({name, startIndex, endIndex}) => ({
      name,
      startIndex,
      endIndex,
    })),
    [{name: 'inside.adjusted-outside', startIndex: 0, endIndex: 1}],
  )

  const final = await assertParity({
    source,
    ...range,
    querySource: String.raw`
      ((identifier) @outside.final
        (#eq? @outside.final "a")
        ${moveToNextIdentifier}
        (#set! capture.final))
      ((identifier) @inside.after (#eq? @inside.after "b"))
    `,
  })
  assert.deepEqual(
    final.map((capture) => capture.name),
    ['inside.after'],
  )

  const shy = await assertParity({
    source,
    ...range,
    querySource: String.raw`
      ((identifier) @outside.claim
        (#eq? @outside.claim "a")
        ${moveToNextIdentifier})
      ((identifier) @inside.shy
        (#eq? @inside.shy "b")
        (#set! capture.shy))
    `,
  })
  assert.deepEqual(
    shy.map((capture) => capture.name),
    ['inside.shy'],
  )

  const rangeWithData = await assertParity({
    source,
    ...range,
    querySource: String.raw`
      ((identifier) @_IGNORE_.outside
        (#eq? @_IGNORE_.outside "a")
        ${moveToNextIdentifier}
        (#set! marker yes))
      ((identifier) @inside.with-data
        (#eq? @inside.with-data "b")
        (#is? test.rangeWithData "marker yes"))
    `,
  })
  assert.deepEqual(rangeWithData, [])
})

test('matches structural, relative-node, field, and row predicates', async () => {
  const source = [
    'const head = target(one, two);',
    'const tail = other(three);',
    'const rows = true ||',
    '  false;',
    'const inline = true || false;',
    '',
  ].join('\n')
  const captures = await assertParity({
    source,
    querySource: String.raw`
      ("const" @first (#is? test.first))
      (";" @last (#is? test.last))
      ((identifier) @first.of.type
        (#is? test.typeAt "parent arguments")
        (#is? test.firstOfType))
      ((identifier) @last.of.type
        (#is? test.typeAt "parent arguments")
        (#is? test.lastOfType))
      ((identifier) @type.at
        (#is? test.typeAt "parent arguments"))
      ((identifier) @text.at
        (#is? test.textAt "parent.previousNamedSibling target"))
      ((identifier) @match.at
        (#is? test.matchAt "parent.previousNamedSibling ^(?:target|other)$"))
      ((identifier) @field.function
        (#is? test.field function))
      ("||" @row.last-text (#is? test.lastTextOnRow))
      ((false) @row.first-text (#is? test.firstTextOnRow))
      ((false) @row.starts-with-parent
        (#is? test.startsOnSameRowAs parent.startPosition))
      ((false) @row.ends-with-parent
        (#is? test.endsOnSameRowAs parent.endPosition))
    `,
  })
  assertNameCounts(captures, {
    first: 4,
    last: 4,
    'first.of.type': 2,
    'last.of.type': 2,
    'type.at': 3,
    'text.at': 2,
    'match.at': 3,
    'field.function': 2,
    'row.last-text': 1,
    'row.first-text': 1,
    'row.starts-with-parent': 1,
    'row.ends-with-parent': 2,
  })
})

test('matches root, type, error, ancestor, and descendant predicates', async () => {
  const source = [
    'function outer(first, second) {',
    '  return first + second;',
    '}',
    'const broken = ;',
    '',
  ].join('\n')
  const captures = await assertParity({
    source,
    querySource: String.raw`
      ((program) @root (#is? test.root))
      ((program) @has.error (#is? test.hasError))
      ((identifier) @type.identifier (#is? test.type "identifier string"))
      ((identifier) @inside.function
        (#is? test.descendantOfType function_declaration))
      ((identifier) @child.parameters
        (#is? test.childOfType formal_parameters))
      ((identifier) @near.parameters
        (#is? test.ancestorTypeNearerThan "formal_parameters function_declaration"))
      ((function_declaration) @contains.return
        (#is? test.ancestorOfType return_statement))
      ((formal_parameters) @parent.of.identifier
        (#is? test.parentOfType identifier))
      ((identifier) @not.direct.program-child
        (#is-not? test.childOfType program))
    `,
  })
  assertNameCounts(captures, {
    root: 1,
    'has.error': 1,
    'type.identifier': 6,
    'inside.function': 5,
    'child.parameters': 2,
    'near.parameters': 2,
    'contains.return': 1,
    'parent.of.identifier': 1,
    'not.direct.program-child': 6,
  })
})

test('matches range data, config, injection, and unknown custom no-op behavior', async () => {
  const source = [
    'function special(a, b) {}',
    'function ordinary(c, d) {}',
    'const enabled = true;',
    '',
  ].join('\n')
  const captures = await assertParity({
    source,
    scopeConfig: {
      'feature.enabled': true,
      'feature.limit': 42,
      'feature.mode': 'strict',
    },
    injectionDepth: 2,
    querySource: String.raw`
      ((function_declaration) @_IGNORE_.special
        (#match? @_IGNORE_.special "^function special")
        (#set! special "yes"))
      ((",") @range.ancestor
        (#is? test.descendantOfNodeWithData "special yes"))
      ((true) @_IGNORE_.range (#set! exact "yes"))
      ((true) @range.same (#is? test.rangeWithData "exact yes"))
      ((true) @config.boolean (#is? test.config "feature.enabled true"))
      ((true) @config.number (#is? test.config "feature.limit 42"))
      ((true) @config.string (#is? test.config "feature.mode strict"))
      ((identifier) @injection
        (#eq? @injection "enabled")
        (#is? test.injection))
      ((identifier) @unknown.custom
        (#eq? @unknown.custom "enabled")
        (#is? vendor.unknown "ignored")
        (#custom-predicate! @unknown.custom "payload"))
    `,
  })
  assertNameCounts(captures, {
    'range.ancestor': 1,
    'range.same': 1,
    'config.boolean': 1,
    'config.number': 1,
    'config.string': 1,
    injection: 1,
    'unknown.custom': 1,
    '_IGNORE_.special': 0,
    '_IGNORE_.range': 0,
  })
})
