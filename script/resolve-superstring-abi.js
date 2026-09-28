'use strict'

const fs = require('node:fs')
const path = require('node:path')

const packageRoot = path.resolve(__dirname, '..')
const relativeHeader = path.join('src', 'bindings', 'snapshot-lease.h')

function candidateRoots() {
  const roots = []
  for (const value of [
    process.env.SUPERSTRING_SOURCE_PATH,
    process.env.SUPERSTRING_PATH,
  ]) {
    if (value) roots.push(path.resolve(value))
  }
  roots.push(
    path.resolve(packageRoot, '..', 'superstring'),
    path.resolve(packageRoot, 'superstring'),
    path.resolve(packageRoot, 'node_modules', '@lumine-code', 'superstring'),
  )
  try {
    roots.push(
      path.dirname(
        require.resolve('@lumine-code/superstring/package.json', {
          paths: [packageRoot],
        }),
      ),
    )
  } catch {
    // Resolution is optional while the flat sibling checkout is available.
  }
  return [...new Set(roots)]
}

function headerPathFor(root) {
  if (path.basename(root) === 'snapshot-lease.h') return root
  return path.join(root, relativeHeader)
}

function findHeader() {
  for (const root of candidateRoots()) {
    const headerPath = headerPathFor(root)
    if (fs.existsSync(headerPath)) return fs.realpathSync(headerPath)
  }
  throw new Error(
    'Unable to locate Superstring SnapshotLease. Place Superstring beside ' +
      'document-engine, install the pinned @lumine-code/superstring dependency, ' +
      'or set SUPERSTRING_SOURCE_PATH.',
  )
}

function validateHeader(headerPath) {
  const source = fs.readFileSync(headerPath, 'utf8')
  const required = [
    /#define\s+SUPERSTRING_SNAPSHOT_LEASE_ABI_VERSION\s+1u\b/,
    /SUPERSTRING_SNAPSHOT_SOURCE_TYPE_TAG/,
    /SUPERSTRING_SNAPSHOT_LEASE_TYPE_TAG/,
    /typedef struct SuperstringSnapshotLease\b/,
    /typedef struct SuperstringSnapshotLeaseFunctions\b/,
    /superstring_snapshot_lease_acquire\s*\(/,
  ]
  for (const pattern of required) {
    if (!pattern.test(source)) {
      throw new Error(
        `Superstring header does not satisfy SnapshotLease: ${headerPath}`,
      )
    }
  }
}

const headerPath = findHeader()
validateHeader(headerPath)

switch (process.argv[2]) {
  case '--print-include':
    process.stdout.write(path.dirname(headerPath))
    break
  case '--print-header':
    process.stdout.write(headerPath)
    break
  case '--check':
  case undefined:
    process.stdout.write(
      `${JSON.stringify(
        {
          abiVersion: 1,
          headerPath,
        },
        null,
        2,
      )}\n`,
    )
    break
  default:
    throw new Error(`Unknown argument: ${process.argv[2]}`)
}
