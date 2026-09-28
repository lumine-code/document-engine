'use strict'

const assert = require('node:assert/strict')
const test = require('node:test')
const {DocumentSession} = require('..')
const {loadSuperstring} = require('./helpers')

const {TextBuffer} = loadSuperstring()

async function publish(session, buffer, revision, foldUpdates) {
  const snapshot = buffer.getSnapshot()
  const result = session.applyRevision(
    snapshot,
    new Uint32Array(0),
    revision,
    foldUpdates,
  )
  snapshot.destroy()
  return result
}

function unpackRenderPlan(plan) {
  assert.ok(plan.lineIds instanceof Float64Array)
  assert.ok(plan.lineDescriptors instanceof Uint32Array)
  assert.ok(plan.tags instanceof Int32Array)
  assert.equal(plan.lineDescriptorStride, 5)
  return Array.from(plan.lineIds, (id, lineIndex) => {
    const offset = lineIndex * plan.lineDescriptorStride
    const textStart = plan.lineDescriptors[offset]
    const textLength = plan.lineDescriptors[offset + 1]
    const tagsStart = plan.lineDescriptors[offset + 2]
    const tagsLength = plan.lineDescriptors[offset + 3]
    return {
      id,
      lineText: plan.text.slice(textStart, textStart + textLength),
      tags: Array.from(plan.tags.subarray(tagsStart, tagsStart + tagsLength)),
      softWrapIndent:
        plan.lineDescriptors[offset + 4] === 0xffffffff
          ? -1
          : plan.lineDescriptors[offset + 4],
    }
  })
}

test('builds viewport-only render lines and preserves the compatibility shape', async () => {
  const buffer = new TextBuffer('ab\tc\r\ndef\n😀z')
  const session = new DocumentSession()
  const view = session.createDisplayView({wrapColumn: 4, tabLength: 2})
  await publish(session, buffer, 1)

  const plan = view.buildRenderPlan(0, 20)
  assert.equal(plan.bufferRevision, 1)
  assert.equal(plan.syntaxRevision, 0)
  assert.deepEqual(
    plan.lines.map((line) => line.lineText),
    ['ab  ', 'c', 'def', '😀z'],
  )
  for (const line of plan.lines) {
    assert.equal(typeof line.id, 'number')
    assert.ok(line.tags instanceof Int32Array)
    assert.equal(typeof line.softWrapIndent, 'number')
  }
  const packedPlan = view.buildRenderPlanPacked(0, 20)
  assert.equal(
    packedPlan.text,
    plan.lines.map((line) => line.lineText).join(''),
  )
  assert.deepEqual(
    unpackRenderPlan(packedPlan),
    plan.lines.map((line) => ({
      ...line,
      tags: Array.from(line.tags),
    })),
  )

  assert.deepEqual(view.bufferToScreen({row: 0, column: 3}, 'backward'), {
    row: 1,
    column: 0,
  })
  assert.deepEqual(view.bufferToScreen({row: 0, column: 3}, 'forward'), {
    row: 1,
    column: 0,
  })
  assert.deepEqual(view.screenToBuffer({row: 1, column: 1}), {
    row: 0,
    column: 4,
  })

  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.fullBufferMaterializations, 0)
  assert.ok(diagnostics.viewportUtf16Copied > 0)
  await session.destroy()
})

test('preserves dynamic continuation indentation after leading whitespace wraps', async () => {
  const buffer = new TextBuffer('            abcdefgh')
  const session = new DocumentSession()
  const view = session.createDisplayView({wrapColumn: 8})
  await publish(session, buffer, 1)

  assert.deepEqual(
    view.buildRenderPlan(0, 20).lines.map((line) => line.lineText),
    ['        ', '    abcd', '    efgh'],
  )
  await session.destroy()
})

test('reports prefix summaries and preserves out-of-range row contracts', async () => {
  const buffer = new TextBuffer('111 111\n222 222\n3\n4\n5\n6\n7\n8')
  const session = new DocumentSession()
  const view = session.createDisplayView({wrapColumn: 4})
  await publish(session, buffer, 1)

  assert.deepEqual(view.getIndexedSummary(0), {
    screenLineCount: 0,
    rightmostScreenPosition: {row: 0, column: 0},
  })
  assert.deepEqual(view.getIndexedSummary(2), {
    screenLineCount: 4,
    rightmostScreenPosition: {row: 0, column: 4},
  })
  assert.equal(view.getIndexedSummary(4).screenLineCount, 6)

  const count = view.getScreenLineCount()
  const rows = Array.from(view.bufferRowsForScreenRows(count - 1, count + 2))
  assert.deepEqual(rows, [7, 8, 9])

  const packed = view.translateScreenColumnBlock(count - 1, count + 2, 1, 3)
  assert.equal(packed.length, 20)
  for (let offset = 5; offset < packed.length; offset += 5) {
    assert.deepEqual(
      Array.from(packed.slice(offset, offset + 5)),
      Array.from(packed.slice(0, 5)),
    )
  }

  await session.destroy()
})

test('projects active folds without moving the public MarkerLayer', async () => {
  const buffer = new TextBuffer('hello\nsecret\nworld')
  const session = new DocumentSession()
  const view = session.createDisplayView()
  await publish(session, buffer, 1)

  view.replaceFolds(1, 1, new Uint32Array([7, 0, 2, 2, 2]))
  let plan = view.buildRenderPlan(0, 10)
  assert.deepEqual(
    plan.lines.map((line) => line.lineText),
    ['he⋯rld'],
  )
  assert.equal(plan.foldGeneration, 1)
  assert.deepEqual(view.bufferToScreen({row: 1, column: 3}, 'backward'), {
    row: 0,
    column: 2,
  })
  assert.deepEqual(view.bufferToScreen({row: 1, column: 3}, 'forward'), {
    row: 0,
    column: 3,
  })

  view.applyFoldDeltas(1, 1, 2, {
    removals: new Uint32Array([7]),
    upserts: new Uint32Array(0),
  })
  plan = view.buildRenderPlan(0, 10)
  assert.deepEqual(
    plan.lines.map((line) => line.lineText),
    ['hello', 'secret', 'world'],
  )
  assert.throws(
    () => view.applyFoldDeltas(1, 1, 3, {}),
    (error) => error.code === 'ERR_FOLD_GENERATION_MISMATCH',
  )
  const diagnostics = view.getDiagnostics()
  assert.equal(diagnostics.foldDeltaCount, 1)
  assert.ok(diagnostics.foldDeltaMilliseconds >= 0)
  assert.ok(diagnostics.foldDeltaMaximumMilliseconds >= 0)

  await session.destroy()
})

test('omits syntax scopes fully hidden by a fold across soft wraps', async () => {
  const buffer = new TextBuffer('abcdefghijklmnopqrstuvwxyz0123456789')
  const session = new DocumentSession()
  const view = session.createDisplayView({wrapColumn: 8})
  await publish(session, buffer, 1)
  view.replaceFolds(1, 1, new Uint32Array([7, 0, 6, 0, 24]))
  view.replaceHighlightRanges(
    1,
    0,
    new Uint32Array([259, 15, 16, 0, 0, 261, 28, 29, 1, 0, 263, 4, 26, 2, 0]),
  )

  const tags = view
    .buildRenderPlan(0, 20)
    .lines.flatMap((line) => Array.from(line.tags))
  assert.equal(tags.includes(-259), false)
  assert.equal(tags.includes(-260), false)
  assert.equal(tags.includes(-261), true)
  assert.equal(tags.includes(-262), true)
  assert.equal(tags.includes(-263), true)
  assert.equal(tags.includes(-264), true)
  await session.destroy()
})

test('applies packed fold splices before upserts and removals', async () => {
  const buffer = new TextBuffer('head\none\ntwo\nend')
  const session = new DocumentSession()
  const view = session.createDisplayView()
  await publish(session, buffer, 1)
  view.replaceFolds(1, 1, new Uint32Array([7, 1, 0, 2, 3]))

  buffer.setTextInRange(
    {start: {row: 0, column: 0}, end: {row: 0, column: 0}},
    'new\n',
  )
  const snapshot = buffer.getSnapshot()
  await session.applyRevision(
    snapshot,
    new Uint32Array([0, 0, 0, 0, 0, 0, 1, 0]),
    2,
  )
  snapshot.destroy()
  view.applyFoldDeltas(2, 1, 2, {
    splices: new Uint32Array([0, 0, 0, 0, 1, 0]),
  })

  assert.deepEqual(
    view.buildRenderPlan(0, 10).lines.map((line) => line.lineText),
    ['new', 'head', '⋯', 'end'],
  )
  await session.destroy()
})

test('publishes revision and full fold reset atomically', async () => {
  const buffer = new TextBuffer('left\nmiddle\nright')
  const session = new DocumentSession()
  const view = session.createDisplayView()
  await publish(session, buffer, 1)

  buffer.setText('left!\nmiddle\nright')
  await publish(session, buffer, 2, [
    {
      view,
      fromGeneration: 0,
      toGeneration: 1,
      ranges: new Uint32Array([1, 0, 4, 2, 1]),
    },
  ])

  const plan = view.buildRenderPlan(0, 10)
  assert.equal(plan.bufferRevision, 2)
  assert.equal(plan.foldGeneration, 1)
  assert.deepEqual(
    plan.lines.map((line) => line.lineText),
    ['left⋯ight'],
  )
  await session.destroy()
})

test('atomically clears the last fold when an edit invalidates its marker', async () => {
  const buffer = new TextBuffer('abc\ndef')
  const session = new DocumentSession()
  const view = session.createDisplayView()
  await publish(session, buffer, 1)
  view.replaceFolds(1, 1, new Uint32Array([7, 0, 1, 1, 2]))

  buffer.setText('a')
  const snapshot = buffer.getSnapshot()
  await session.applyRevision(
    snapshot,
    new Uint32Array([0, 1, 1, 3, 0, 1, 0, 1]),
    2,
    [
      {
        view,
        fromGeneration: 1,
        toGeneration: 2,
        ranges: new Uint32Array(0),
      },
    ],
  )
  snapshot.destroy()

  const plan = view.buildRenderPlan(0, 2)
  assert.equal(plan.foldGeneration, 2)
  assert.deepEqual(
    plan.lines.map((line) => line.lineText),
    ['a'],
  )
  assert.equal(view.getDiagnostics().activeFoldCount, 0)
  await session.destroy()
})

test('supports packed point and range projection', async () => {
  const buffer = new TextBuffer('abc\ndef')
  const session = new DocumentSession()
  const view = session.createDisplayView()
  await publish(session, buffer, 1)

  assert.deepEqual(
    Array.from(view.bufferToScreen(new Uint32Array([0, 2, 1, 1]))),
    [0, 2, 1, 1],
  )
  assert.deepEqual(
    Array.from(view.projectBufferRanges(new Uint32Array([0, 1, 1, 2]))),
    [0, 1, 1, 2],
  )
  await session.destroy()
})

test('publishes text and layout synchronously while syntax-derived scopes trail', async () => {
  const buffer = new TextBuffer('abc\ntail')
  const session = new DocumentSession({workerDelayMs: 40})
  const view = session.createDisplayView()
  await publish(session, buffer, 1)
  view.replaceHighlightRanges(
    1,
    0,
    new Uint32Array([259, 0, 3, 0, 0, 261, 4, 8, 1, 0]),
  )
  assert.deepEqual(
    Array.from(view.buildRenderPlan(0, 1).lines[0].tags),
    [-259, 3, -260],
  )

  buffer.setTextInRange(
    {start: {row: 0, column: 1}, end: {row: 0, column: 1}},
    '!',
  )
  const snapshot = buffer.getSnapshot()
  const pending = session.applyRevision(
    snapshot,
    new Uint32Array([0, 1, 0, 1, 0, 1, 0, 2]),
    2,
  )
  snapshot.destroy()

  const plan = view.buildRenderPlan(0, 2)
  assert.equal(plan.bufferRevision, 2)
  assert.equal(plan.syntaxRevision, 0)
  assert.equal(plan.lines[0].lineText, 'a!bc')
  assert.deepEqual(Array.from(plan.lines[0].tags), [4])
  assert.deepEqual(Array.from(plan.lines[1].tags), [-261, 4, -262])
  assert.deepEqual(view.bufferToScreen({row: 1, column: 2}), {
    row: 1,
    column: 2,
  })

  await pending
  await session.destroy()
})

test('extends only the synthetic language envelope across an intersecting edit', async () => {
  const buffer = new TextBuffer('abc')
  const session = new DocumentSession({workerDelayMs: 40})
  const view = session.createDisplayView()
  await publish(session, buffer, 1)
  view.replaceHighlightRanges(
    1,
    0,
    new Uint32Array([259, 0, 3, 0, 0, 261, 0, 3, 1, 0]),
  )
  assert.deepEqual(
    Array.from(view.buildRenderPlan(0, 1).lines[0].tags),
    [-259, -261, 3, -262, -260],
  )

  buffer.setTextInRange(
    {start: {row: 0, column: 1}, end: {row: 0, column: 1}},
    '!',
  )
  const snapshot = buffer.getSnapshot()
  const pending = session.applyRevision(
    snapshot,
    new Uint32Array([0, 1, 0, 1, 0, 1, 0, 2]),
    2,
  )
  snapshot.destroy()

  const plan = view.buildRenderPlan(0, 1)
  assert.equal(plan.lines[0].lineText, 'a!bc')
  assert.deepEqual(Array.from(plan.lines[0].tags), [-259, 4, -260])

  await pending
  await session.destroy()
})

test('updates CRLF line metadata for a multi-edit batch without a full byte scan', async () => {
  const buffer = new TextBuffer('aa\r\nbb\ncc')
  const session = new DocumentSession()
  const view = session.createDisplayView()
  await publish(session, buffer, 1)

  buffer.setTextInRange(
    {start: {row: 2, column: 1}, end: {row: 2, column: 1}},
    'Y',
  )
  buffer.setTextInRange(
    {start: {row: 0, column: 1}, end: {row: 0, column: 1}},
    'X',
  )
  const snapshot = buffer.getSnapshot()
  await session.applyRevision(
    snapshot,
    new Uint32Array([0, 1, 0, 1, 0, 1, 0, 2, 2, 1, 2, 1, 2, 1, 2, 2]),
    2,
  )
  snapshot.destroy()

  assert.deepEqual(
    view.buildRenderPlan(0, 10).lines.map((line) => line.lineText),
    ['aXa', 'bb', 'cYc'],
  )
  const diagnostics = session.getDiagnostics()
  assert.equal(diagnostics.synchronousFullAnalyses, 0)
  assert.ok(diagnostics.synchronousIncrementalAnalyses >= 1)
  assert.ok(diagnostics.snapshotLinesRead > 0)
  await session.destroy()
})

test('rejects cross-session and stale fold resets', async () => {
  const firstBuffer = new TextBuffer('one')
  const secondBuffer = new TextBuffer('two')
  const first = new DocumentSession()
  const second = new DocumentSession()
  const foreignView = second.createDisplayView()
  await publish(first, firstBuffer, 1)
  await publish(second, secondBuffer, 1)

  firstBuffer.setText('next')
  const snapshot = firstBuffer.getSnapshot()
  assert.throws(
    () =>
      first.applyRevision(snapshot, new Uint32Array(0), 2, [
        {
          view: foreignView,
          fromGeneration: 0,
          toGeneration: 1,
          ranges: new Uint32Array(0),
        },
      ]),
    (error) => error.code === 'ERR_INVALID_FOLD_UPDATES',
  )
  snapshot.destroy()

  await first.destroy()
  await second.destroy()
})
