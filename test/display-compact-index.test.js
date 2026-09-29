'use strict'

const assert = require('node:assert/strict')
const test = require('node:test')
const {DocumentSession} = require('..')
const {loadSuperstring} = require('./helpers')

const {TextBuffer} = loadSuperstring()

async function publish(session, buffer) {
  const snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()
}

test('keeps long-line display state compact across multiple views', async () => {
  const length = 250_000
  const buffer = new TextBuffer('\t'.repeat(length))
  const session = new DocumentSession()
  await publish(session, buffer)

  const views = Array.from({length: 4}, () =>
    session.createDisplayView({
      wrapColumn: 500,
      tabLength: 4,
    }),
  )

  for (const view of views) {
    assert.equal(view.buildRenderPlan(0, 50).lines.length, 50)
    const diagnostics = view.getDiagnostics()
    assert.equal(diagnostics.sourceUtf16Length, length)
    assert.equal(diagnostics.screenRowCount, 2_000)
    assert.equal(diagnostics.displaySpanCount, diagnostics.screenRowCount)
    assert.equal(diagnostics.peakLogicalSegments, 1)
    assert.equal(diagnostics.peakRowSpans, 1)
    assert.ok(diagnostics.retainedBytes < 2 * 1024 * 1024)
    assert.ok(diagnostics.layoutUnitsScanned < length * 1.01)
  }

  await session.destroy()
})

test('indexes a one-million-unit line without retaining per-unit records', async () => {
  const length = 1_000_000
  const buffer = new TextBuffer('x'.repeat(length))
  const session = new DocumentSession()
  await publish(session, buffer)
  const view = session.createDisplayView({wrapColumn: 500})

  const plan = view.buildRenderPlan(0, 25)
  const diagnostics = view.getDiagnostics()
  assert.equal(plan.lines.length, 25)
  assert.equal(diagnostics.screenRowCount, 2_000)
  assert.equal(diagnostics.displaySpanCount, 2_000)
  assert.ok(diagnostics.retainedBytes < 2 * 1024 * 1024)
  assert.ok(diagnostics.layoutUnitsScanned < length * 1.01)

  await session.destroy()
})

test('does not rescan every source unit when a wrapped line contains a fold', async () => {
  const length = 250_000
  const buffer = new TextBuffer(
    'alpha beta-gamma/delta '.repeat(11_000).slice(0, length),
  )
  const session = new DocumentSession()
  await publish(session, buffer)
  const view = session.createDisplayView({
    wrapColumn: 500,
    wrapBoundaryMode: 'standard',
  })
  const foldStart = Math.floor(length / 3)
  view.replaceFolds(
    1,
    1,
    new Uint32Array([1, 0, foldStart, 0, foldStart + 1000]),
  )

  view.buildRenderPlan(0, 50)
  const diagnostics = view.getDiagnostics()
  assert.ok(diagnostics.layoutUnitsScanned < length * 1.05)
  assert.ok(diagnostics.peakLogicalSegments >= 3)

  await session.destroy()
})
