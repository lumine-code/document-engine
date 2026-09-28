'use strict'

const {performance} = require('node:perf_hooks')
const {DocumentSession} = require('..')
const {TextBuffer} = require('../../superstring')

function percentile(samples, fraction) {
  const sorted = [...samples].sort((left, right) => left - right)
  return sorted[
    Math.min(sorted.length - 1, Math.floor(sorted.length * fraction))
  ]
}

async function runCase({length, views, samples, coldRuns}) {
  const buffer = new TextBuffer('\t'.repeat(length))
  const session = new DocumentSession()
  const snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()

  const cold = []
  const warm = []
  let diagnostics = []
  for (let run = 0; run < coldRuns; run++) {
    const runDiagnostics = []
    for (let index = 0; index < views; index++) {
      const view = session.createDisplayView({wrapColumn: 500, tabLength: 4})
      const started = performance.now()
      view.buildRenderPlan(0, 50)
      cold.push(performance.now() - started)
      for (let sample = 0; sample < samples; sample++) {
        const warmStarted = performance.now()
        view.buildRenderPlan(0, 50)
        warm.push(performance.now() - warmStarted)
      }
      runDiagnostics.push(view.getDiagnostics())
      view.destroy()
    }
    diagnostics = runDiagnostics
  }

  await session.destroy()
  return {
    length,
    views,
    cold: {median: percentile(cold, 0.5), p95: percentile(cold, 0.95)},
    warm: {median: percentile(warm, 0.5), p95: percentile(warm, 0.95)},
    retainedBytes: diagnostics.reduce(
      (sum, item) => sum + item.retainedBytes,
      0,
    ),
    displaySpans: diagnostics.reduce(
      (sum, item) => sum + item.displaySpanCount,
      0,
    ),
    layoutUnitsScanned: diagnostics.reduce(
      (sum, item) => sum + item.layoutUnitsScanned,
      0,
    ),
  }
}

async function runSynchronousRevisionCase(length) {
  const buffer = new TextBuffer('x'.repeat(length))
  const session = new DocumentSession()
  let snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()

  const middle = Math.floor(length / 2)
  buffer.setTextInRange(
    {start: {row: 0, column: middle}, end: {row: 0, column: middle}},
    '!',
  )
  snapshot = buffer.getSnapshot()
  let started = performance.now()
  let pending = session.applyRevision(
    snapshot,
    new Uint32Array([0, middle, 0, middle, 0, middle, 0, middle + 1]),
    2,
  )
  const singleEditCallMilliseconds = performance.now() - started
  snapshot.destroy()
  await pending

  const first = Math.floor(length / 4)
  const second = Math.floor((length * 3) / 4) + 1
  buffer.setTextInRange(
    {start: {row: 0, column: second}, end: {row: 0, column: second}},
    '?',
  )
  buffer.setTextInRange(
    {start: {row: 0, column: first}, end: {row: 0, column: first}},
    '?',
  )
  snapshot = buffer.getSnapshot()
  started = performance.now()
  pending = session.applyRevision(
    snapshot,
    new Uint32Array([
      0,
      first,
      0,
      first,
      0,
      first,
      0,
      first + 1,
      0,
      second,
      0,
      second,
      0,
      second + 1,
      0,
      second + 2,
    ]),
    3,
  )
  const multiEditCallMilliseconds = performance.now() - started
  snapshot.destroy()
  await pending
  const diagnostics = session.getDiagnostics()
  await session.destroy()
  return {
    length,
    singleEditCallMilliseconds,
    multiEditCallMilliseconds,
    synchronousIncrementalAnalyses: diagnostics.synchronousIncrementalAnalyses,
    synchronousLineIndexAnalyses: diagnostics.synchronousLineIndexAnalyses,
    synchronousFullAnalyses: diagnostics.synchronousFullAnalyses,
    synchronousAnalysisMilliseconds:
      diagnostics.synchronousAnalysisMilliseconds,
  }
}

async function main() {
  const samples = Number.parseInt(process.env.LUMINE_BENCH_SAMPLES || '20', 10)
  const coldRuns = Number.parseInt(
    process.env.LUMINE_BENCH_COLD_RUNS || '5',
    10,
  )
  const results = []
  for (const length of [250_000, 1_000_000]) {
    for (const views of [1, 2, 4]) {
      results.push(await runCase({length, views, samples, coldRuns}))
    }
  }
  const synchronousRevisions = await runSynchronousRevisionCase(1_000_000)
  process.stdout.write(
    `${JSON.stringify(
      {
        schemaVersion: 2,
        results,
        synchronousRevisions,
      },
      null,
      2,
    )}\n`,
  )
}

main().catch((error) => {
  console.error(error)
  process.exitCode = 1
})
