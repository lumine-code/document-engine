'use strict'

const fs = require('node:fs')
const path = require('node:path')
const {spawnSync} = require('node:child_process')
const documentEngine = require('..')
const TreeSitter = require('web-tree-sitter')

const workspaceRoot = path.resolve(__dirname, '..', '..')
const Season = require(path.join(workspaceRoot, 'season'))
const {TextBuffer} = require(path.join(workspaceRoot, 'superstring'))
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
const ScopeResolver = require(
  path.join(workspaceRoot, 'lumine', 'src', 'scope-resolver.js'),
)

const languageCache = new Map()
const differentialPackages = new Set([
  'language-gfm',
  'language-html',
  'language-ipython',
  'language-javascript',
  'language-python',
  'language-rust',
  'language-typescript',
  'language-vue',
])

function needsDifferentialOracle(descriptor) {
  return (
    process.env.LUMINE_QUERY_FLEET_DIFFERENTIAL_ALL === '1' ||
    differentialPackages.has(descriptor.label.split('/')[0])
  )
}

function grammarDescriptors() {
  const result = []
  for (const entry of fs.readdirSync(workspaceRoot, {withFileTypes: true})) {
    if (!entry.isDirectory() || !entry.name.startsWith('language-')) continue
    const grammarDirectory = path.join(workspaceRoot, entry.name, 'grammars')
    if (!fs.existsSync(grammarDirectory)) continue
    for (const fileName of fs.readdirSync(grammarDirectory)) {
      if (!/\.(?:json|cson)$/.test(fileName)) continue
      const descriptorPath = path.join(grammarDirectory, fileName)
      let descriptor
      try {
        descriptor = Season.readFileSync(descriptorPath)
      } catch {
        continue
      }
      if (descriptor?.type !== 'tree-sitter') continue
      const config = descriptor.treeSitter
      if (!config || (config.runtime ?? 'wasm') !== 'wasm') continue
      const wasmPath = path.join(grammarDirectory, config.grammar)
      const languageName = path
        .basename(config.grammar, '.wasm')
        .replace(/^tree-sitter-/, '')
        .replaceAll('-', '_')
      const queryPaths = {}
      for (const [queryType, configuredPaths] of Object.entries(config)) {
        if (!queryType.endsWith('Query')) continue
        queryPaths[queryType] = (
          Array.isArray(configuredPaths) ? configuredPaths : [configuredPaths]
        ).map((queryPath) => path.join(grammarDirectory, queryPath))
      }
      result.push({
        label: `${entry.name}/${fileName}`,
        languageId: descriptor.scopeName,
        wasmPath,
        languageName,
        languageSegment: config.languageSegment,
        queryPaths,
        grammarDirectory,
        fileTypes: descriptor.fileTypes ?? [],
      })
    }
  }
  return result
}

async function applyRevision(session, source) {
  const buffer = new TextBuffer(source)
  const snapshot = buffer.getSnapshot()
  try {
    await session.applyRevision(snapshot, new Uint32Array(0), 1)
  } finally {
    snapshot.destroy()
  }
}

function walkFiles(directory, output) {
  if (!fs.existsSync(directory)) return
  for (const entry of fs.readdirSync(directory, {withFileTypes: true})) {
    const entryPath = path.join(directory, entry.name)
    if (entry.isDirectory()) walkFiles(entryPath, output)
    else if (entry.isFile()) output.push(entryPath)
  }
}

function fixtureFor(descriptor) {
  const repository = path.dirname(descriptor.grammarDirectory)
  const files = []
  walkFiles(path.join(repository, 'spec', 'fixtures'), files)
  walkFiles(path.join(repository, 'test', 'fixtures'), files)
  const fileTypes = descriptor.fileTypes.map((value) =>
    String(value).toLowerCase(),
  )
  const scored = files
    .map((filePath) => {
      let size
      try {
        size = fs.statSync(filePath).size
      } catch {
        return null
      }
      if (size === 0 || size > 128 * 1024) return null
      const base = path.basename(filePath).toLowerCase()
      const extension = path.extname(base).slice(1)
      let score = 0
      if (fileTypes.includes(base)) score += 100
      if (fileTypes.includes(extension)) score += 80
      if (
        base.includes(path.basename(descriptor.wasmPath, '.wasm').toLowerCase())
      )
        score += 10
      return {filePath, score, size}
    })
    .filter(Boolean)
    .sort(
      (left, right) =>
        right.score - left.score ||
        left.size - right.size ||
        left.filePath.localeCompare(right.filePath),
    )
  if (scored.length === 0) return {path: null, source: ''}
  const selected = scored[0].filePath
  return {path: selected, source: fs.readFileSync(selected, 'utf8')}
}

function createLegacyBuffer(text) {
  const lineStarts = [0]
  for (let index = 0; index < text.length; index++) {
    if (text.charCodeAt(index) === 10) lineStarts.push(index + 1)
  }
  const lineEnd = (row) => {
    const next = lineStarts[row + 1]
    let end = next === undefined ? text.length : next - 1
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
      let row = lineStarts.length - 1
      while (row > 0 && lineStarts[row] > index) row--
      return clipPosition({row, column: index - lineStarts[row]})
    },
    clipPosition,
    lineForRow(row) {
      if (row < 0 || row >= lineStarts.length) return undefined
      return text.slice(lineStarts[row], lineEnd(row))
    },
  }
}

async function languageFor(wasmPath) {
  let promise = languageCache.get(wasmPath)
  if (!promise) {
    promise = TreeSitter.Language.load(wasmPath)
    languageCache.set(wasmPath, promise)
  }
  return promise
}

function querySourceFor(descriptor, queryType) {
  let source = descriptor.queryPaths[queryType]
    .map((queryPath) => fs.readFileSync(queryPath, 'utf8'))
    .join('\n')
  if (descriptor.languageSegment) {
    source = source.replaceAll('._LANG_', `.${descriptor.languageSegment}`)
  }
  return source
}

async function legacyCaptures(descriptor, queryType, source) {
  const language = await languageFor(descriptor.wasmPath)
  const parser = new TreeSitter.Parser()
  parser.setLanguage(language)
  const tree = parser.parse(source)
  const query = new TreeSitter.Query(
    language,
    querySourceFor(descriptor, queryType),
  )
  const resolver = new ScopeResolver({
    buffer: createLegacyBuffer(source),
    depth: 0,
    grammar: {scopeName: descriptor.languageId},
    languageMode: {config: global.lumine.config},
  })
  try {
    const captures = []
    for (const capture of query.captures(tree.rootNode)) {
      const range = resolver.store(capture, {boundaries: false})
      if (!range) continue
      captures.push([
        capture.name,
        capture.patternIndex,
        range.startPosition.row,
        range.startPosition.column,
        range.endPosition.row,
        range.endPosition.column,
        range.startIndex,
        range.endIndex,
      ])
    }
    return captures
  } finally {
    resolver.destroy()
    query.delete()
    tree.delete()
    parser.delete()
  }
}

function nativeCaptures(result) {
  const captures = []
  for (
    let offset = 0;
    offset < result.captures.length;
    offset += result.captureStride
  ) {
    captures.push([
      result.captureNames[result.captures[offset]],
      result.captures[offset + 1],
      result.captures[offset + 3],
      result.captures[offset + 4],
      result.captures[offset + 5],
      result.captures[offset + 6],
      result.captures[offset + 7],
      result.captures[offset + 8],
    ])
  }
  return captures
}

function firstDifference(expected, actual) {
  const length = Math.max(expected.length, actual.length)
  for (let index = 0; index < length; index++) {
    if (JSON.stringify(expected[index]) !== JSON.stringify(actual[index])) {
      return {
        index,
        expected: expected[index],
        actual: actual[index],
        expectedContext: expected.slice(Math.max(0, index - 3), index + 4),
        actualContext: actual.slice(Math.max(0, index - 3), index + 4),
      }
    }
  }
  return null
}

async function main() {
  if (
    !process.env.LUMINE_QUERY_FLEET_CHILD &&
    !process.env.LUMINE_QUERY_FLEET_FILTER
  ) {
    return runBatchedParent()
  }
  await TreeSitter.Parser.init()
  const filter = process.env.LUMINE_QUERY_FLEET_FILTER
  const start = Number(process.env.LUMINE_QUERY_FLEET_START ?? 0)
  const end = Number(
    process.env.LUMINE_QUERY_FLEET_END ?? Number.MAX_SAFE_INTEGER,
  )
  const descriptors = grammarDescriptors()
    .filter((descriptor) => !filter || descriptor.label.includes(filter))
    .slice(start, end)
  const failures = []
  const regexFallbacks = []
  let queryFiles = 0
  let unresolved = 0
  let unresolvedRegex = 0
  let scopePredicates = 0
  let customPredicates = 0
  let smokeQueries = 0
  let smokeCaptures = 0
  let differentialQueries = 0
  let differentialCaptures = 0
  let descriptorsWithFixture = 0
  for (const descriptor of descriptors) {
    if (process.env.LUMINE_QUERY_FLEET_PROGRESS)
      process.stderr.write(`checking ${descriptor.label}\n`)
    const session = new documentEngine.DocumentSession()
    try {
      session.setLanguage({
        languageId: descriptor.languageId,
        runtime: 'wasm',
        wasmPath: descriptor.wasmPath,
        languageName: descriptor.languageName,
        languageSegment: descriptor.languageSegment,
        queryPaths: descriptor.queryPaths,
      })
      const fixture = fixtureFor(descriptor)
      if (fixture.path) descriptorsWithFixture++
      await applyRevision(session, fixture.source)
      const diagnostics = session.getDiagnostics()
      queryFiles += diagnostics.queryFileCount
      unresolved += diagnostics.queryUnresolvedPredicates
      unresolvedRegex += diagnostics.queryUnresolvedRegexPredicates
      scopePredicates += diagnostics.queryScopePredicates
      customPredicates += diagnostics.queryCustomPredicates
      if (diagnostics.queryUnresolvedRegexPredicates > 0) {
        for (const queryType of Object.keys(descriptor.queryPaths)) {
          const captureResult = session.getQueryCaptures(queryType)
          for (const [
            patternIndex,
            metadata,
          ] of captureResult.propertySets.entries()) {
            for (const predicate of metadata.unresolvedPredicates) {
              regexFallbacks.push({
                label: descriptor.label,
                queryType,
                patternIndex,
                operator: predicate.operator,
                pattern: predicate.operands.at(-1)?.value,
              })
            }
          }
        }
      }
      for (const queryType of Object.keys(descriptor.queryPaths)) {
        if (queryType === 'injectionsQuery') continue
        const actual = nativeCaptures(
          session.getQueryCaptures(queryType, 0, 0xffffffff, {
            resolveScopes: true,
          }),
        )
        smokeQueries++
        smokeCaptures += actual.length
        if (!needsDifferentialOracle(descriptor)) continue
        const expected = await legacyCaptures(
          descriptor,
          queryType,
          fixture.source,
        )
        differentialQueries++
        differentialCaptures += expected.length
        const difference = firstDifference(expected, actual)
        if (difference) {
          failures.push({
            label: descriptor.label,
            code: 'ERR_QUERY_DIFFERENTIAL_MISMATCH',
            message: `${queryType} differs at capture ${difference.index}`,
            fixture: fixture.path,
            difference,
            diagnostics: session.getDiagnostics(),
          })
          break
        }
      }
    } catch (error) {
      failures.push({
        label: descriptor.label,
        code: error.code,
        message: error.message,
        diagnostics: session.getDiagnostics(),
      })
    } finally {
      await session.destroy()
    }
  }

  const summary = {
    descriptors: descriptors.length,
    queryFiles,
    unresolvedPredicates: unresolved,
    unresolvedRegexPredicates: unresolvedRegex,
    scopePredicates,
    customPredicates,
    smokeQueries,
    smokeCaptures,
    differentialQueries,
    differentialCaptures,
    descriptorsWithFixture,
    regexFallbacks,
    failures: failures.length,
  }
  const serializedSummary = `${JSON.stringify(summary, null, 2)}\n`
  if (process.env.LUMINE_QUERY_FLEET_CHILD) {
    fs.writeSync(process.stdout.fd, serializedSummary)
  } else {
    process.stdout.write(serializedSummary)
  }
  for (const failure of failures) {
    console.error(`\n${failure.label}: ${failure.code}: ${failure.message}`)
    if (failure.diagnostics.queryLastErrorPath) {
      console.error(
        `${failure.diagnostics.queryLastErrorPath}:` +
          `${failure.diagnostics.queryLastErrorLine}:` +
          `${failure.diagnostics.queryLastErrorColumn}`,
      )
    }
    if (failure.fixture) console.error(`fixture: ${failure.fixture}`)
    if (failure.difference) console.error(JSON.stringify(failure.difference))
  }
  if (failures.length > 0) process.exitCode = 1
}

function runBatchedParent() {
  const descriptorCount = grammarDescriptors().length
  // web-tree-sitter retains enough per-language Wasm state that a handful of
  // the largest grammars can exhaust V8's Zone allocator in one process. One
  // descriptor per child keeps the fleet check deterministic and also proves
  // that every grammar can initialize in a cold process.
  const batchSize = Number(process.env.LUMINE_QUERY_FLEET_BATCH_SIZE ?? 1)
  if (!Number.isInteger(batchSize) || batchSize < 1) {
    throw new RangeError(
      'LUMINE_QUERY_FLEET_BATCH_SIZE must be a positive integer',
    )
  }
  const aggregate = {
    descriptors: 0,
    queryFiles: 0,
    unresolvedPredicates: 0,
    unresolvedRegexPredicates: 0,
    scopePredicates: 0,
    customPredicates: 0,
    smokeQueries: 0,
    smokeCaptures: 0,
    differentialQueries: 0,
    differentialCaptures: 0,
    descriptorsWithFixture: 0,
    regexFallbacks: [],
    failures: 0,
  }
  for (let start = 0; start < descriptorCount; start += batchSize) {
    const end = Math.min(descriptorCount, start + batchSize)
    const child = spawnSync(process.execPath, [__filename], {
      cwd: __dirname,
      encoding: 'utf8',
      maxBuffer: 16 * 1024 * 1024,
      env: {
        ...process.env,
        LUMINE_QUERY_FLEET_CHILD: '1',
        LUMINE_QUERY_FLEET_START: String(start),
        LUMINE_QUERY_FLEET_END: String(end),
      },
    })
    if (child.error) throw child.error
    let summary
    try {
      summary = JSON.parse(child.stdout)
    } catch (error) {
      throw new Error(
        `Query fleet batch ${start}-${end} produced invalid JSON: ${child.stdout}\n${child.stderr}`,
        {cause: error},
      )
    }
    for (const key of [
      'descriptors',
      'queryFiles',
      'unresolvedPredicates',
      'unresolvedRegexPredicates',
      'scopePredicates',
      'customPredicates',
      'smokeQueries',
      'smokeCaptures',
      'differentialQueries',
      'differentialCaptures',
      'descriptorsWithFixture',
      'failures',
    ])
      aggregate[key] += summary[key]
    aggregate.regexFallbacks.push(...summary.regexFallbacks)
    if (
      child.stderr &&
      (child.status !== 0 || process.env.LUMINE_QUERY_FLEET_PROGRESS)
    )
      process.stderr.write(child.stderr)
    if (child.status !== 0 && summary.failures === 0) {
      throw new Error(
        `Query fleet batch ${start}-${end} exited ${child.status}`,
      )
    }
  }
  console.log(JSON.stringify(aggregate, null, 2))
  if (aggregate.failures > 0) process.exitCode = 1
}

main().catch((error) => {
  console.error(error)
  process.exitCode = 1
})
