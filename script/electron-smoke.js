'use strict'

const path = require('node:path')
const {app} = require('electron')
const documentEngine = require('..')
const superstring = require(path.resolve(__dirname, '..', '..', 'superstring'))

async function main() {
  await app.whenReady()
  const buffer = new superstring.TextBuffer('alpha\tbeta\n')
  const session = new documentEngine.DocumentSession()
  const view = session.createDisplayView({wrapColumn: 8, tabLength: 4})
  const snapshot = buffer.getSnapshot()
  try {
    const revision = await session.applyRevision(
      snapshot,
      new Uint32Array(0),
      1,
    )
    const plan = view.buildRenderPlan(0, 10)
    if (revision.accepted !== true || plan.lines.length === 0) {
      throw new Error('Electron ABI smoke did not publish a render plan')
    }
  } finally {
    snapshot.destroy()
    view.destroy()
    await session.destroy()
  }
  const leases = superstring._getSnapshotLeaseDiagnostics().activeConsumerLeases
  if (leases !== 0)
    throw new Error(`Electron ABI smoke leaked ${leases} snapshot lease(s)`)
  process.stdout.write(
    `${JSON.stringify({electron: process.versions.electron, leases})}\n`,
  )
  app.quit()
}

main().catch((error) => {
  process.stderr.write(`${error.stack ?? error.message}\n`)
  app.exit(1)
})
