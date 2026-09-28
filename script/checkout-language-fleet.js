'use strict'

const fs = require('node:fs')
const path = require('node:path')
const {spawn} = require('node:child_process')

const packageRoot = path.resolve(__dirname, '..')
const workspaceRoot = path.resolve(packageRoot, '..')

function catalogRepositories(catalog) {
  const repositories = new Map()
  for (const entry of catalog) {
    const name = entry?.metadata?.name
    const source = entry?.source
    const sha = entry?.resolvedSha
    if (typeof name !== 'string' || !name.startsWith('language-')) continue
    const match = /^lumine-code\/([a-z0-9._-]+)$/i.exec(source ?? '')
    if (!match || !/^[0-9a-f]{40}$/i.test(sha ?? '')) {
      throw new Error(
        `Catalog entry ${JSON.stringify(name)} has no immutable Lumine source`,
      )
    }
    const previous = repositories.get(match[1])
    if (previous && previous.sha !== sha) {
      throw new Error(`${source} appears with conflicting resolved SHAs`)
    }
    repositories.set(match[1], {name: match[1], sha: sha.toLowerCase(), source})
  }
  return [...repositories.values()].sort((left, right) =>
    left.name.localeCompare(right.name),
  )
}

function run(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      stdio: 'inherit',
      windowsHide: true,
      ...options,
    })
    child.once('error', reject)
    child.once('close', (code, signal) => {
      if (code === 0) resolve()
      else reject(new Error(`${command} exited ${code ?? signal}`))
    })
  })
}

async function checkoutRepository(repository, root = workspaceRoot) {
  const target = path.resolve(root, repository.name)
  const relative = path.relative(path.resolve(root), target)
  if (!relative || relative.startsWith('..') || path.isAbsolute(relative)) {
    throw new Error(`Refusing checkout outside the workspace: ${target}`)
  }
  if (fs.existsSync(target)) {
    const current = await capture('git', ['-C', target, 'rev-parse', 'HEAD'])
    if (current.trim().toLowerCase() === repository.sha) return
    throw new Error(
      `Checkout already exists at a different revision: ${target}`,
    )
  }

  fs.mkdirSync(target)
  await run('git', ['-C', target, 'init', '--quiet'])
  await run('git', [
    '-C',
    target,
    'remote',
    'add',
    'origin',
    `https://github.com/${repository.source}.git`,
  ])
  await run('git', [
    '-C',
    target,
    'fetch',
    '--quiet',
    '--depth=1',
    'origin',
    repository.sha,
  ])
  await run('git', [
    '-C',
    target,
    'checkout',
    '--quiet',
    '--detach',
    'FETCH_HEAD',
  ])
}

function capture(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      encoding: 'utf8',
      stdio: ['ignore', 'pipe', 'pipe'],
      windowsHide: true,
      ...options,
    })
    let stdout = ''
    let stderr = ''
    child.stdout.on('data', (chunk) => {
      stdout += chunk
    })
    child.stderr.on('data', (chunk) => {
      stderr += chunk
    })
    child.once('error', reject)
    child.once('close', (code) => {
      if (code === 0) resolve(stdout)
      else reject(new Error(`${command} exited ${code}: ${stderr}`))
    })
  })
}

async function mapConcurrent(values, concurrency, action) {
  let next = 0
  async function worker() {
    while (next < values.length) {
      const index = next++
      await action(values[index], index)
    }
  }
  await Promise.all(
    Array.from({length: Math.min(concurrency, values.length)}, worker),
  )
}

async function main() {
  const catalogPath = path.resolve(
    process.argv[2] ?? path.join(workspaceRoot, 'packages', 'index.json'),
  )
  const catalog = JSON.parse(fs.readFileSync(catalogPath, 'utf8'))
  const repositories = catalogRepositories(catalog)
  const concurrency = Number(process.env.LUMINE_FLEET_CHECKOUT_CONCURRENCY ?? 8)
  if (!Number.isInteger(concurrency) || concurrency < 1 || concurrency > 32) {
    throw new RangeError(
      'LUMINE_FLEET_CHECKOUT_CONCURRENCY must be between 1 and 32',
    )
  }
  await mapConcurrent(repositories, concurrency, (repository) =>
    checkoutRepository(repository, workspaceRoot),
  )
  process.stdout.write(
    `Checked out ${repositories.length} language repositories at catalog SHAs.\n`,
  )
}

if (require.main === module) {
  main().catch((error) => {
    process.stderr.write(`${error.stack ?? error.message}\n`)
    process.exitCode = 1
  })
}

module.exports = {catalogRepositories, checkoutRepository, mapConcurrent}
