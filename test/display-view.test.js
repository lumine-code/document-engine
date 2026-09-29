'use strict'

process.env.LUMINE_DOCUMENT_ENGINE_ENABLE_TEST_FAULTS = '1'

const assert = require('node:assert/strict')
const test = require('node:test')
const {DocumentSession, _createFailingSnapshotLeaseForTest} = require('..')
delete process.env.LUMINE_DOCUMENT_ENGINE_ENABLE_TEST_FAULTS
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

function normalizedLines(view) {
  return view
    .buildRenderPlan(0, view.getScreenLineCount() + 1)
    .lines.map(({lineText, tags, softWrapIndent}) => ({
      lineText,
      tags: Array.from(tags),
      softWrapIndent,
    }))
}

function compareDisplayViews(incremental, rebuilt, buffer) {
  assert.deepEqual(normalizedLines(incremental), normalizedLines(rebuilt))
  for (let row = 0; row < buffer.getLineCount(); row++) {
    const length = buffer.lineLengthForRow(row)
    for (let column = 0; column <= length; column++) {
      for (const clip of ['backward', 'closest', 'forward']) {
        assert.deepEqual(
          incremental.bufferToScreen({row, column}, clip),
          rebuilt.bufferToScreen({row, column}, clip),
          `buffer point [${row}, ${column}] ${clip}`,
        )
      }
    }
  }
  const screenRows = incremental.getScreenLineCount()
  assert.equal(screenRows, rebuilt.getScreenLineCount())
  for (let row = 0; row < screenRows; row++) {
    const length = Math.max(
      incremental.lineLengthForScreenRow(row),
      rebuilt.lineLengthForScreenRow(row),
    )
    for (let column = 0; column <= length + 1; column++) {
      for (const clip of ['backward', 'closest', 'forward']) {
        assert.deepEqual(
          incremental.screenToBuffer({row, column}, clip),
          rebuilt.screenToBuffer({row, column}, clip),
          `screen point [${row}, ${column}] ${clip}`,
        )
      }
    }
  }
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

test('renders fragmented snapshot storage like the same contiguous text', async () => {
  const fragmented = new TextBuffer(
    `${'alpha beta\tgamma/delta '.repeat(8)}\n${'tail '.repeat(20)}`,
  )
  fragmented.setTextInRange(
    {start: {row: 0, column: 17}, end: {row: 0, column: 17}},
    '界',
  )
  fragmented.setTextInRange(
    {start: {row: 1, column: 11}, end: {row: 1, column: 11}},
    'e\u0301',
  )
  fragmented.setTextInRange(
    {start: {row: 0, column: 73}, end: {row: 0, column: 76}},
    'XYZ',
  )
  const contiguous = new TextBuffer(fragmented.getText())
  const fragmentedSession = new DocumentSession()
  const contiguousSession = new DocumentSession()
  const options = {
    wrapColumn: 17,
    tabLength: 4,
    wrapBoundaryMode: 'standard',
    characterWidthProfile: {doubleWidth: 2},
  }
  const fragmentedView = fragmentedSession.createDisplayView(options)
  const contiguousView = contiguousSession.createDisplayView(options)

  await publish(fragmentedSession, fragmented, 1)
  await publish(contiguousSession, contiguous, 1)
  const folds = new Uint32Array([7, 0, 31, 0, 67])
  fragmentedView.replaceFolds(1, 1, folds)
  contiguousView.replaceFolds(1, 1, folds)

  const normalizePlan = (plan) =>
    plan.lines.map(({lineText, tags, softWrapIndent}) => ({
      lineText,
      tags: Array.from(tags),
      softWrapIndent,
    }))
  assert.deepEqual(
    normalizePlan(fragmentedView.buildRenderPlan(0, 100)),
    normalizePlan(contiguousView.buildRenderPlan(0, 100)),
  )
  for (const point of [
    {row: 0, column: 0},
    {row: 0, column: 17},
    {row: 0, column: 68},
    {row: 0, column: 150},
    {row: 1, column: 12},
  ]) {
    assert.deepEqual(
      fragmentedView.bufferToScreen(point),
      contiguousView.bufferToScreen(point),
    )
  }

  await fragmentedSession.destroy()
  await contiguousSession.destroy()
})

test('matches contiguous storage across randomized fragment layouts and seeks', async () => {
  let randomState = 0x6d2b79f5
  const random = (maximum) => {
    randomState = (Math.imul(randomState, 1664525) + 1013904223) >>> 0
    return randomState % maximum
  }
  const insertions = ['x', '\t', '界', 'e\u0301']

  for (let iteration = 0; iteration < 12; iteration++) {
    const fragmented = new TextBuffer(
      `${'alpha beta/gamma '.repeat(6)}\n${'middle delta '.repeat(6)}\n${'tail '.repeat(12)}`,
    )
    for (let edit = 0; edit < 24; edit++) {
      const row = random(3)
      const column = random(fragmented.lineLengthForRow(row) + 1)
      fragmented.setTextInRange(
        {start: {row, column}, end: {row, column}},
        insertions[random(insertions.length)],
      )
    }

    const contiguous = new TextBuffer(fragmented.getText())
    const fragmentedSession = new DocumentSession()
    const contiguousSession = new DocumentSession()
    const options = {
      wrapColumn: 9 + random(10),
      tabLength: 2 + random(4),
      wrapBoundaryMode: 'standard',
      characterWidthProfile: {doubleWidth: 2},
    }
    const fragmentedView = fragmentedSession.createDisplayView(options)
    const contiguousView = contiguousSession.createDisplayView(options)

    try {
      await publish(fragmentedSession, fragmented, 1)
      await publish(contiguousSession, contiguous, 1)
      const folds = new Uint32Array([7, 0, 19, 0, 43, 9, 1, 7, 2, 11])
      fragmentedView.replaceFolds(1, 1, folds)
      contiguousView.replaceFolds(1, 1, folds)

      const normalizePlan = (plan) =>
        plan.lines.map(({lineText, tags, softWrapIndent}) => ({
          lineText,
          tags: Array.from(tags),
          softWrapIndent,
        }))
      assert.deepEqual(
        normalizePlan(fragmentedView.buildRenderPlan(0, 1000)),
        normalizePlan(contiguousView.buildRenderPlan(0, 1000)),
        `render plan iteration ${iteration}`,
      )

      const points = new Uint32Array(128)
      for (let index = 0; index < points.length; index += 2) {
        const row = random(3)
        points[index] = row
        points[index + 1] = random(fragmented.lineLengthForRow(row) + 1)
      }
      assert.deepEqual(
        Array.from(fragmentedView.bufferToScreen(points)),
        Array.from(contiguousView.bufferToScreen(points)),
        `random seeks iteration ${iteration}`,
      )
    } finally {
      await fragmentedSession.destroy()
      await contiguousSession.destroy()
    }
  }
})

test('incrementally reflows one edited source line like a forced full rebuild', async () => {
  let randomState = 0x13579bdf
  const random = (maximum) => {
    randomState = (Math.imul(randomState, 1664525) + 1013904223) >>> 0
    return randomState % maximum
  }
  const initial =
    '  alpha\tbeta😀gamma e\u0301 delta / tail words\n' +
    'second\tline with 界 and many words to wrap\n' +
    'third-line/with-boundaries and a trailing token'
  const incrementalBuffer = new TextBuffer(initial)
  const rebuiltBuffer = new TextBuffer(initial)
  const incrementalSession = new DocumentSession()
  const rebuiltSession = new DocumentSession()
  const options = {
    wrapColumn: 13,
    tabLength: 4,
    softWrapHangingIndent: 2,
    wrapBoundaryMode: 'standard',
    characterWidthProfile: {doubleWidth: 2},
  }
  const incrementalView = incrementalSession.createDisplayView(options)
  const rebuiltView = rebuiltSession.createDisplayView(options)
  const replacements = ['', 'x', '\t', '界', 'e\u0301', '😀']

  try {
    await publish(incrementalSession, incrementalBuffer, 1)
    await publish(rebuiltSession, rebuiltBuffer, 1)
    compareDisplayViews(incrementalView, rebuiltView, incrementalBuffer)

    for (let revision = 2; revision <= 31; revision++) {
      const row = random(incrementalBuffer.getLineCount())
      const lineLength = incrementalBuffer.lineLengthForRow(row)
      const startColumn = random(lineLength + 1)
      const deletedLength = Math.min(random(4), lineLength - startColumn)
      const endColumn = startColumn + deletedLength
      const replacement = replacements[random(replacements.length)]
      const range = {
        start: {row, column: startColumn},
        end: {row, column: endColumn},
      }
      incrementalBuffer.setTextInRange(range, replacement)
      rebuiltBuffer.setTextInRange(range, replacement)

      let snapshot = incrementalBuffer.getSnapshot()
      await incrementalSession.applyRevision(
        snapshot,
        new Uint32Array([
          row,
          startColumn,
          row,
          endColumn,
          row,
          startColumn,
          row,
          startColumn + replacement.length,
        ]),
        revision,
      )
      snapshot.destroy()
      snapshot = rebuiltBuffer.getSnapshot()
      // Omitting edit metadata deliberately keeps this view on the full-rebuild
      // oracle while publishing the same immutable snapshot.
      await rebuiltSession.applyRevision(snapshot, new Uint32Array(0), revision)
      snapshot.destroy()

      compareDisplayViews(incrementalView, rebuiltView, incrementalBuffer)
    }

    const incrementalDiagnostics = incrementalView.getDiagnostics()
    const rebuiltDiagnostics = rebuiltView.getDiagnostics()
    assert.equal(incrementalDiagnostics.indexIncrementalUpdateCount, 30)
    assert.equal(incrementalDiagnostics.indexIncrementalFallbackCount, 0)
    assert.equal(rebuiltDiagnostics.indexIncrementalUpdateCount, 0)
    assert.ok(incrementalDiagnostics.indexIncrementalRowsReused > 0)
  } finally {
    await incrementalSession.destroy()
    await rebuiltSession.destroy()
  }
})

test('bounds incremental reflow work by the edited soft-wrap suffix', async () => {
  const length = 20_000
  const buffer = new TextBuffer('x'.repeat(length))
  const session = new DocumentSession()
  const view = session.createDisplayView({wrapColumn: 100})

  try {
    await publish(session, buffer, 1)
    view.buildRenderPlan(0, 1)
    const initial = view.getDiagnostics()
    assert.equal(initial.indexRebuildCount, 1)

    const edit = async (revision, column, inserted) => {
      const oldLength = inserted ? 0 : 1
      const newLength = inserted ? 1 : 0
      buffer.setTextInRange(
        {
          start: {row: 0, column},
          end: {row: 0, column: column + oldLength},
        },
        inserted ? 'y' : '',
      )
      const snapshot = buffer.getSnapshot()
      await session.applyRevision(
        snapshot,
        new Uint32Array([
          0,
          column,
          0,
          column + oldLength,
          0,
          column,
          0,
          column + newLength,
        ]),
        revision,
      )
      snapshot.destroy()
      view.buildRenderPlan(0, 1)
    }

    await edit(2, Math.floor(length / 2), true)
    let diagnostics = view.getDiagnostics()
    assert.equal(diagnostics.indexIncrementalUpdateCount, 1)
    assert.equal(diagnostics.indexRebuildCount, 1)
    assert.ok(diagnostics.layoutUnitsScanned < length * 0.55)
    assert.ok(diagnostics.indexIncrementalRowsReused > 90)

    const previousUnits = diagnostics.indexIncrementalLayoutUnitsScanned
    await edit(3, buffer.lineLengthForRow(0) - 10, true)
    diagnostics = view.getDiagnostics()
    assert.equal(diagnostics.indexIncrementalUpdateCount, 2)
    assert.ok(
      diagnostics.indexIncrementalLayoutUnitsScanned - previousUnits < 250,
    )
  } finally {
    await session.destroy()
  }
})

test('falls back to a full display rebuild when an edit changes line count', async () => {
  const buffer = new TextBuffer('abc def ghi')
  const session = new DocumentSession()
  const view = session.createDisplayView({wrapColumn: 5})
  try {
    await publish(session, buffer, 1)
    view.buildRenderPlan(0, 10)
    buffer.setTextInRange(
      {start: {row: 0, column: 3}, end: {row: 0, column: 3}},
      '\n',
    )
    const snapshot = buffer.getSnapshot()
    await session.applyRevision(
      snapshot,
      new Uint32Array([0, 3, 0, 3, 0, 3, 1, 0]),
      2,
    )
    snapshot.destroy()
    assert.deepEqual(
      view.buildRenderPlan(0, 10).lines.map((line) => line.lineText),
      ['abc', ' def ', ' ghi'],
    )
    const diagnostics = view.getDiagnostics()
    assert.equal(diagnostics.indexIncrementalUpdateCount, 0)
    assert.equal(diagnostics.indexIncrementalFallbackCount, 1)
    assert.equal(diagnostics.indexRebuildCount, 2)
  } finally {
    await session.destroy()
  }
})

test('does not publish display geometry after a SnapshotLease chunk failure', async () => {
  for (const scenario of [
    {
      name: 'incremental update',
      replacement: 'X',
      edits: new Uint32Array([0, 5, 0, 5, 0, 5, 0, 6]),
      expectedLines: ['alphaX beta', 'tail'],
    },
    {
      name: 'full rebuild',
      replacement: '\n',
      edits: new Uint32Array([0, 5, 0, 5, 0, 5, 1, 0]),
      expectedLines: ['alpha', ' beta', 'tail'],
    },
  ]) {
    const buffer = new TextBuffer('alpha beta\ntail')
    const session = new DocumentSession()
    const view = session.createDisplayView({wrapColumn: 80})
    try {
      await publish(session, buffer, 1)
      assert.deepEqual(
        view.buildRenderPlan(0, 10).lines.map((line) => line.lineText),
        ['alpha beta', 'tail'],
      )
      const before = view.getDiagnostics()

      buffer.setTextInRange(
        {start: {row: 0, column: 5}, end: {row: 0, column: 5}},
        scenario.replacement,
      )
      const upstream = buffer.getSnapshot()
      const failing = _createFailingSnapshotLeaseForTest(upstream, 0)
      upstream.destroy()
      await session.applyRevision(failing, scenario.edits, 2)
      failing.destroy()

      assert.throws(
        () => view.buildRenderPlan(0, 10),
        (error) =>
          error.code === 'ERR_SNAPSHOT_LEASE' &&
          /chunk read/i.test(error.message),
        scenario.name,
      )
      const failed = view.getDiagnostics()
      assert.equal(failed.cachedBufferRevision, 0)
      assert.equal(failed.targetBufferRevision, 2)
      assert.equal(failed.displayRevision, before.displayRevision)
      assert.equal(failed.indexRebuildCount, before.indexRebuildCount)
      assert.equal(
        failed.indexIncrementalUpdateCount,
        before.indexIncrementalUpdateCount,
      )
      assert.equal(failed.sourceUtf16Length, before.sourceUtf16Length)
      assert.equal(failed.screenRowCount, before.screenRowCount)

      const recovery = buffer.getSnapshot()
      await session.applyRevision(recovery, new Uint32Array(0), 3)
      recovery.destroy()
      const recovered = view.buildRenderPlan(0, 10)
      assert.equal(recovered.bufferRevision, 3)
      assert.deepEqual(
        recovered.lines.map((line) => line.lineText),
        scenario.expectedLines,
      )
      assert.equal(view.getDiagnostics().cachedBufferRevision, 3)
    } finally {
      await session.destroy()
    }
  }
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
