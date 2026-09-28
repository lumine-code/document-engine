'use strict'

const path = require('node:path')
const {performance} = require('node:perf_hooks')
const {DocumentSession} = require('..')
const {TextBuffer} = require('../test/helpers').loadSuperstring()

const workspaceRoot = path.resolve(__dirname, '..', '..')
const grammarRoot = path.join(workspaceRoot, 'language-javascript', 'grammars')
const iterations = Number(process.env.LUMINE_QUERY_BENCH_ITERATIONS ?? 12)
const rows = Number(process.env.LUMINE_QUERY_BENCH_ROWS ?? 4000)

function median(values) {
  const sorted = [...values].sort((left, right) => left - right)
  const middle = Math.floor(sorted.length / 2)
  return sorted.length % 2 === 0
    ? (sorted[middle - 1] + sorted[middle]) / 2
    : sorted[middle]
}

async function measure(callback) {
  const samples = []
  for (let iteration = 0; iteration < iterations; iteration++) {
    const startedAt = performance.now()
    await callback(iteration)
    samples.push(performance.now() - startedAt)
  }
  return {median: median(samples), samples}
}

async function main() {
  const source =
    Array.from(
      {length: rows},
      (_, row) =>
        `export function value${row} (input) { return input + ${row} }`,
    ).join('\n') + '\n'
  const buffer = new TextBuffer(source)
  const session = new DocumentSession()
  session.setLanguage({
    languageId: 'source.js',
    runtime: 'wasm',
    wasmPath: path.join(grammarRoot, 'javascript.wasm'),
    languageName: 'javascript',
    queryPaths: Object.fromEntries(
      ['highlights', 'folds', 'indents', 'locals', 'tags'].map((name) => [
        `${name}Query`,
        [path.join(grammarRoot, `javascript-${name}.scm`)],
      ]),
    ),
  })

  let revision = 0
  const parse = await measure(async () => {
    const snapshot = buffer.getSnapshot()
    try {
      await session.applyRevision(snapshot, new Uint32Array(0), ++revision)
    } finally {
      snapshot.destroy()
    }
  })
  const afterParse = session.getDiagnostics()

  const viewport = await measure(() => {
    session.getQueryCaptures('highlightsQuery', 1900, 2000)
  })
  const full = await measure(() => {
    session.getQueryCaptures('highlightsQuery', 0, 0xffffffff)
  })
  const afterQueries = session.getDiagnostics()

  if (afterParse.queryProgramsExecuted !== 0) {
    throw new Error(
      `parse eagerly executed ${afterParse.queryProgramsExecuted} non-injection queries`,
    )
  }
  if (afterQueries.queryProgramsExecuted < 1) {
    throw new Error('on-demand query execution was not recorded')
  }

  process.stdout.write(
    `${JSON.stringify(
      {
        schemaVersion: 1,
        rows,
        iterations,
        parseMilliseconds: parse,
        viewportHighlightsMilliseconds: viewport,
        fullHighlightsMilliseconds: full,
        queryProgramsAfterParse: afterParse.queryProgramsExecuted,
        queryProgramsAfterQueries: afterQueries.queryProgramsExecuted,
      },
      null,
      2,
    )}\n`,
  )
  await session.destroy()
}

main().catch((error) => {
  process.stderr.write(`${error.stack ?? error}\n`)
  process.exitCode = 1
})
