'use strict'

const assert = require('node:assert/strict')
const fs = require('node:fs')
const path = require('node:path')
const test = require('node:test')
const documentEngine = require('..')
const superstring = require('../../superstring')

const lumineRoot = path.resolve(__dirname, '..', '..', 'lumine')
const hasLegacyOracle = fs.existsSync(
  path.join(lumineRoot, 'src', 'display-layer.js'),
)

let LegacyTextBuffer
let textUtils
let layoutProfileForDisplayLayer
if (hasLegacyOracle) {
  const installedSuperstring = require.resolve('@lumine-code/superstring', {
    paths: [lumineRoot],
  })
  require(installedSuperstring)
  require.cache[installedSuperstring].exports = superstring
  LegacyTextBuffer = require(path.join(lumineRoot, 'src', 'text-buffer'))
  textUtils = require(path.join(lumineRoot, 'src', 'text-utils'))
  const controller = require(
    path.join(lumineRoot, 'src', 'document-engine-controller'),
  )
  layoutProfileForDisplayLayer = controller.layoutProfileForDisplayLayer
}

function point(value) {
  return {row: value.row, column: value.column}
}

function widthRatio(character, profile) {
  if (textUtils.isKoreanCharacter(character)) return profile.korean
  if (textUtils.isHalfWidthCharacter(character)) return profile.halfWidth
  if (textUtils.isDoubleWidthCharacter(character)) return profile.doubleWidth
  return profile.default
}

async function buildPair({text, options, folds = []}) {
  const buffer = new LegacyTextBuffer({text})
  const ratioForCharacter = (character) =>
    widthRatio(character, options.characterWidthProfile)
  ratioForCharacter.documentEngineCharacterWidthProfile = () => ({
    ...options.characterWidthProfile,
  })
  let isWrapBoundary
  if (options.wrapBoundaryMode === 'standard') {
    isWrapBoundary = textUtils.isWrapBoundary
  } else if (options.wrapBoundaryMode === 'none') {
    isWrapBoundary = () => false
    isWrapBoundary.documentEngineWrapBoundaryMode = 'none'
  }
  const legacy = buffer.addDisplayLayer({
    invisibles: options.invisibles,
    tabLength: options.tabLength,
    softWrapColumn: options.wrapColumn || Infinity,
    softWrapHangingIndent: options.softWrapHangingIndent,
    atomicSoftTabs: options.atomicSoftTabs,
    foldCharacter: options.foldCharacter,
    isWrapBoundary,
    ratioForCharacter,
  })
  const packedFolds = new Uint32Array(folds.length * 5)
  folds.forEach((range, index) => {
    legacy.foldBufferRange(range)
    packedFolds.set([index + 1, ...range[0], ...range[1]], index * 5)
  })

  const session = new documentEngine.DocumentSession()
  const native = session.createDisplayView(layoutProfileForDisplayLayer(legacy))
  const snapshot = buffer.buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()
  if (folds.length > 0) native.replaceFolds(1, 1, packedFolds)

  return {
    buffer,
    legacy,
    native,
    session,
    async destroy() {
      buffer.destroy()
      await session.destroy()
    },
  }
}

function compareLines(pair) {
  const expected = pair.legacy
    .getScreenLines(0, pair.legacy.getScreenLineCount())
    .map(({lineText, tags, softWrapIndent}) => ({
      lineText,
      tags: Array.from(tags),
      softWrapIndent,
    }))
  const actual = pair.native
    .buildRenderPlan(0, Number.MAX_SAFE_INTEGER)
    .lines.map(({lineText, tags, softWrapIndent}) => ({
      lineText,
      tags: Array.from(tags),
      softWrapIndent,
    }))
  assert.deepEqual(actual, expected)
}

function compareMappings(pair) {
  const clips = ['backward', 'closest', 'forward']
  const packedBufferPoints = []
  const packedExpectedByClip = new Map(clips.map((clip) => [clip, []]))
  for (let row = 0; row < pair.buffer.getLineCount(); row++) {
    const lineLength = pair.buffer.lineForRow(row).length
    for (let column = 0; column <= lineLength + 2; column++) {
      packedBufferPoints.push(row, column)
      for (const clipDirection of clips) {
        const expected = point(
          pair.legacy.translateBufferPosition({row, column}, {clipDirection}),
        )
        packedExpectedByClip
          .get(clipDirection)
          .push(expected.row, expected.column)
        const actual = pair.native.bufferToScreen({row, column}, clipDirection)
        assert.deepEqual(
          actual,
          expected,
          `buffer [${row}, ${column}] ${clipDirection}`,
        )
      }
    }
  }
  const packedInput = Uint32Array.from(packedBufferPoints)
  for (const clipDirection of clips) {
    assert.deepEqual(
      Array.from(pair.native.bufferToScreen(packedInput, clipDirection)),
      packedExpectedByClip.get(clipDirection),
      `packed buffer points ${clipDirection}`,
    )
  }

  const screenLineCount = pair.legacy.getScreenLineCount()
  assert.equal(pair.native.getScreenLineCount(), screenLineCount)
  assert.deepEqual(
    pair.native.getRightmostScreenPosition(),
    point(pair.legacy.getRightmostScreenPosition()),
  )
  for (let row = 0; row < screenLineCount; row++) {
    assert.equal(
      pair.native.lineLengthForScreenRow(row),
      pair.legacy.lineLengthForScreenRow(row),
    )
  }
  assert.deepEqual(
    Array.from(pair.native.bufferRowsForScreenRows(0, screenLineCount)),
    pair.legacy.bufferRowsForScreenRows(0, screenLineCount),
  )
  for (let row = 0; row <= screenLineCount + 1; row++) {
    const lineLength =
      row < screenLineCount ? pair.legacy.lineLengthForScreenRow(row) : 2
    for (let column = 0; column <= lineLength + 2; column++) {
      for (const clipDirection of clips) {
        const expected = point(
          pair.legacy.translateScreenPosition({row, column}, {clipDirection}),
        )
        const actual = pair.native.screenToBuffer({row, column}, clipDirection)
        assert.deepEqual(
          actual,
          expected,
          `screen [${row}, ${column}] ${clipDirection}`,
        )
      }
    }
  }

  if (screenLineCount > 0) {
    const expectedBlock = pair.legacy.translateScreenColumnBlock(
      0,
      screenLineCount - 1,
      1,
      4,
    )
    const packed = pair.native.translateScreenColumnBlock(
      0,
      screenLineCount - 1,
      1,
      4,
    )
    const actualBlock = []
    for (let offset = 0; offset < packed.length; offset += 5) {
      actualBlock.push({
        start: [packed[offset], packed[offset + 1]],
        end: [packed[offset + 2], packed[offset + 3]],
        screenColumn:
          packed[offset + 4] === 0xffffffff ? null : packed[offset + 4],
      })
    }
    assert.deepEqual(
      actualBlock,
      expectedBlock.map(({bufferRange, screenColumn}) => ({
        start: [bufferRange.start.row, bufferRange.start.column],
        end: [bufferRange.end.row, bufferRange.end.column],
        screenColumn,
      })),
    )
  }
}

const parityOptions = {
  wrapColumn: 10,
  tabLength: 4,
  softWrapHangingIndent: 2,
  atomicSoftTabs: true,
  wrapBoundaryMode: 'standard',
  foldCharacter: '⋯',
  characterWidthProfile: {
    default: 1,
    doubleWidth: 2,
    halfWidth: 1,
    korean: 2,
  },
}

test(
  'matches legacy Unicode, CRLF, tabs, standard wrap boundaries and indentation',
  {
    skip: !hasLegacyOracle,
  },
  async () => {
    const pair = await buildPair({
      text: '  alpha\tbeta/中한 😀e\u0301z\r\n\tgamma-delta\n尾',
      options: parityOptions,
    })
    try {
      compareLines(pair)
      compareMappings(pair)
    } finally {
      await pair.destroy()
    }
  },
)

test(
  'matches legacy layout width when a word-boundary wrap carries a tab',
  {
    skip: !hasLegacyOracle,
  },
  async () => {
    for (const {text, lines} of [
      {text: 'a bbbbbb\t x', lines: ['a ', 'bbbbbb       x']},
      {text: 'abc def\t def ', lines: ['abc ', 'def    ', 'def ']},
    ]) {
      const pair = await buildPair({
        text,
        options: {
          ...parityOptions,
          wrapColumn: 12,
          tabLength: 6,
          softWrapHangingIndent: 0,
          wrapBoundaryMode: 'word',
        },
      })
      try {
        assert.deepEqual(
          pair.legacy
            .getScreenLines(0, pair.legacy.getScreenLineCount())
            .map((line) => line.lineText),
          lines,
        )
        compareLines(pair)
        compareMappings(pair)
      } finally {
        await pair.destroy()
      }
    }
  },
)

test(
  'matches legacy same-row, cross-row, adjacent and overlapping folds',
  {
    skip: !hasLegacyOracle,
  },
  async () => {
    const pair = await buildPair({
      text: 'ab\tcdef\nghijkl\nmnopqr\nstuvwx\nyz',
      options: {...parityOptions, wrapColumn: 7, softWrapHangingIndent: 1},
      folds: [
        [
          [0, 2],
          [0, 4],
        ],
        [
          [0, 4],
          [1, 2],
        ],
        [
          [2, 1],
          [4, 2],
        ],
        [
          [3, 0],
          [4, 4],
        ],
      ],
    })
    try {
      compareLines(pair)
      compareMappings(pair)
    } finally {
      await pair.destroy()
    }
  },
)

test(
  'matches packed and scalar mappings at soft-wrap and fold boundaries',
  {
    skip: !hasLegacyOracle,
  },
  async () => {
    const pair = await buildPair({
      text: 'abcdefghijklmnop\nqrstuvwxyz\ntail',
      options: {
        ...parityOptions,
        wrapColumn: 4,
        softWrapHangingIndent: 0,
        wrapBoundaryMode: 'none',
      },
      folds: [
        [
          [0, 4],
          [1, 2],
        ],
      ],
    })
    const points = [
      [0, 0],
      [0, 3],
      [0, 4],
      [0, 5],
      [1, 0],
      [1, 2],
      [1, 4],
      [2, 0],
      [2, 4],
    ]
    const packed = Uint32Array.from(points.flat())
    try {
      for (const clipDirection of ['backward', 'closest', 'forward']) {
        const expected = points.flatMap(([row, column]) => {
          const mapped = pair.legacy.translateBufferPosition(
            {row, column},
            {clipDirection},
          )
          assert.deepEqual(
            pair.native.bufferToScreen({row, column}, clipDirection),
            point(mapped),
            `scalar buffer [${row}, ${column}] ${clipDirection}`,
          )
          return [mapped.row, mapped.column]
        })
        assert.deepEqual(
          Array.from(pair.native.bufferToScreen(packed, clipDirection)),
          expected,
          `packed soft-wrap/fold boundaries ${clipDirection}`,
        )
      }
    } finally {
      await pair.destroy()
    }
  },
)

test(
  'matches legacy tab, space, CR, and EOL invisibles with built-in tags',
  {
    skip: !hasLegacyOracle,
  },
  async () => {
    const pair = await buildPair({
      text: '\talpha  \r\n  beta\t\n',
      options: {
        ...parityOptions,
        wrapColumn: 80,
        invisibles: {tab: '»', space: '·', cr: '¤', eol: '¬'},
      },
    })
    try {
      compareLines(pair)
    } finally {
      await pair.destroy()
    }
  },
)

test(
  'matches legacy across deterministic randomized layout cases',
  {
    skip: !hasLegacyOracle,
  },
  async () => {
    let state = 0x51f15e
    const random = (maximum) => {
      state = (Math.imul(state, 1664525) + 1013904223) >>> 0
      return state % maximum
    }
    const atoms = [
      'a',
      'b',
      ' ',
      '  ',
      '\t',
      '-',
      '/',
      '中',
      '한',
      '😀',
      'e\u0301',
    ]

    for (let iteration = 0; iteration < 30; iteration++) {
      const lines = []
      const lineCount = 1 + random(5)
      for (let row = 0; row < lineCount; row++) {
        let line = ''
        const atomCount = random(14)
        for (let index = 0; index < atomCount; index++)
          line += atoms[random(atoms.length)]
        lines.push(line)
      }
      const text = lines.join(iteration % 2 === 0 ? '\r\n' : '\n')
      const boundaryModes = ['word', 'standard', 'none']
      const options = {
        ...parityOptions,
        wrapColumn: 3 + random(10),
        tabLength: 2 + random(4),
        softWrapHangingIndent: random(3),
        atomicSoftTabs: random(2) === 0,
        wrapBoundaryMode: boundaryModes[random(boundaryModes.length)],
        foldCharacter: random(2) === 0 ? '⋯' : '\uFEFF',
      }
      const points = []
      for (let row = 0; row < lines.length; row++) {
        for (let column = 0; column <= lines[row].length; column++) {
          if (
            column === 0 ||
            !textUtils.isPairedCharacter(lines[row], column - 1)
          ) {
            points.push([row, column])
          }
        }
      }
      const folds = []
      for (let index = 0; index < random(5) && points.length > 1; index++) {
        const left = random(points.length - 1)
        const right = left + 1 + random(points.length - left - 1)
        folds.push([points[left], points[right]])
      }

      const pair = await buildPair({text, options, folds})
      try {
        compareLines(pair)
        compareMappings(pair)
      } catch (error) {
        throw new Error(
          `Random iteration ${iteration}: ${JSON.stringify({text, options, folds})}`,
          {cause: error},
        )
      } finally {
        await pair.destroy()
      }
    }
  },
)

test('keeps render-line ids stable until the corresponding row identity changes', async () => {
  const buffer = new superstring.TextBuffer('first\nsecond\nthird')
  const session = new documentEngine.DocumentSession()
  const view = session.createDisplayView({wrapColumn: 80})

  let snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()
  const before = view.buildRenderPlan(0, 2).lines.map((line) => line.id)
  assert.deepEqual(
    view.buildRenderPlan(0, 2).lines.map((line) => line.id),
    before,
  )

  buffer.setTextInRange(
    {start: {row: 2, column: 5}, end: {row: 2, column: 5}},
    '!',
  )
  snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 2)
  snapshot.destroy()
  assert.deepEqual(
    view.buildRenderPlan(0, 2).lines.map((line) => line.id),
    before,
  )

  await session.destroy()
})

test('keeps unaffected render-line ids stable when an edit shifts them below the viewport', async () => {
  const buffer = new superstring.TextBuffer('first\nsecond\nthird')
  const session = new documentEngine.DocumentSession()
  const view = session.createDisplayView({wrapColumn: 80})

  let snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()
  const before = view.buildRenderPlan(0, 3).lines.map((line) => line.id)

  buffer.setTextInRange(
    {start: {row: 0, column: 0}, end: {row: 0, column: 0}},
    'new\n',
  )
  snapshot = buffer.getSnapshot()
  await session.applyRevision(
    snapshot,
    new Uint32Array([0, 0, 0, 0, 0, 0, 1, 0]),
    2,
  )
  snapshot.destroy()
  const after = view.buildRenderPlan(0, 4).lines.map((line) => line.id)
  assert.deepEqual(after.slice(1), before)

  await session.destroy()
})

test('keeps only fingerprint-identical soft-wrap tail ids after a one-character edit', async () => {
  const buffer = new superstring.TextBuffer('aaaaaaaaaaaaaaaa')
  const session = new documentEngine.DocumentSession()
  const view = session.createDisplayView({wrapColumn: 4})

  let snapshot = buffer.getSnapshot()
  await session.applyRevision(snapshot, new Uint32Array(0), 1)
  snapshot.destroy()
  const before = view.buildRenderPlan(0, 4)
  assert.deepEqual(
    before.lines.map((line) => line.lineText),
    ['aaaa', 'aaaa', 'aaaa', 'aaaa'],
  )

  buffer.setTextInRange(
    {start: {row: 0, column: 1}, end: {row: 0, column: 1}},
    'b',
  )
  snapshot = buffer.getSnapshot()
  await session.applyRevision(
    snapshot,
    new Uint32Array([0, 1, 0, 1, 0, 1, 0, 2]),
    2,
  )
  snapshot.destroy()
  const after = view.buildRenderPlan(0, 5)
  assert.deepEqual(
    after.lines.map((line) => line.lineText),
    ['abaa', 'aaaa', 'aaaa', 'aaaa', 'a'],
  )
  assert.notEqual(after.lines[0].id, before.lines[0].id)
  assert.deepEqual(
    after.lines.slice(1, 3).map((line) => line.id),
    before.lines.slice(1, 3).map((line) => line.id),
  )
  // The old terminal segment is no longer terminal after the insertion, so
  // its display span fingerprint changes even though its text is identical.
  assert.notEqual(after.lines[3].id, before.lines[3].id)
  assert.ok(after.lines[4].id > 0)
  assert.ok(before.lines.every((line) => line.id !== after.lines[4].id))

  await session.destroy()
})
