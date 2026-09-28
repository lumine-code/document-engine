'use strict'

const crypto = require('node:crypto')
const fs = require('node:fs')
const path = require('node:path')
const {spawnSync} = require('node:child_process')
const manifest = require('./native-dependencies.json')

const packageRoot = path.resolve(__dirname, '..')
const dependencyRoot = path.join(packageRoot, '.native-deps')
const cacheRoot = path.join(dependencyRoot, 'cache')
const requestedArch =
  process.env.npm_config_arch ??
  process.env.npm_config_target_arch ??
  process.arch
const architecture = requestedArch === 'x86_64' ? 'x64' : requestedArch
const platformKey = `${process.platform}-${architecture}`
const wasmtimeArtifact = manifest.wasmtime.artifacts[platformKey]

if (!wasmtimeArtifact) {
  throw new Error(`No pinned Wasmtime C API artifact for ${platformKey}`)
}

const treeSitterRoot = path.join(
  dependencyRoot,
  `tree-sitter-${manifest.treeSitter.version}`,
)
const pcre2Root = path.join(dependencyRoot, `pcre2-${manifest.pcre2.version}`)
const wasmtimeRoot = path.join(
  dependencyRoot,
  `wasmtime-${manifest.wasmtime.version}-${platformKey}`,
)

async function main() {
  fs.mkdirSync(cacheRoot, {recursive: true})
  await provision({
    name: `Tree-sitter ${manifest.treeSitter.version}`,
    url: manifest.treeSitter.url,
    sha256: manifest.treeSitter.sha256,
    archivePath: path.join(
      cacheRoot,
      `tree-sitter-${manifest.treeSitter.version}.tar.gz`,
    ),
    destination: treeSitterRoot,
    marker: path.join('lib', 'include', 'tree_sitter', 'api.h'),
  })
  await provision({
    name: `PCRE2 ${manifest.pcre2.version}`,
    url: manifest.pcre2.url,
    sha256: manifest.pcre2.sha256,
    archivePath: path.join(cacheRoot, `pcre2-${manifest.pcre2.version}.tar.gz`),
    destination: pcre2Root,
    marker: path.join('src', 'pcre2.h.generic'),
  })
  provisionPcreHeaders()
  await provision({
    name: `Wasmtime ${manifest.wasmtime.version} C API (${platformKey})`,
    url: `${manifest.wasmtime.baseUrl}/${wasmtimeArtifact.file}`,
    sha256: wasmtimeArtifact.sha256,
    archivePath: path.join(cacheRoot, wasmtimeArtifact.file),
    destination: wasmtimeRoot,
    marker: path.join('include', 'wasmtime.h'),
  })

  switch (process.argv[2]) {
    case '--print-tree-sitter-root':
      process.stdout.write(gypPath(treeSitterRoot))
      break
    case '--print-wasmtime-root':
      process.stdout.write(wasmtimeRoot)
      break
    case '--print-pcre2-root':
      process.stdout.write(gypPath(pcre2Root))
      break
    default:
      break
  }
}

function gypPath(directory) {
  return path.relative(packageRoot, directory).replaceAll(path.sep, '/')
}

function provisionPcreHeaders() {
  for (const [sourceName, destinationName] of [
    ['config.h.generic', 'config.h'],
    ['pcre2.h.generic', 'pcre2.h'],
    ['pcre2_chartables.c.dist', 'pcre2_chartables.c'],
  ]) {
    const source = path.join(pcre2Root, 'src', sourceName)
    const destination = path.join(pcre2Root, 'src', destinationName)
    if (
      !fs.existsSync(destination) ||
      hashFile(destination) !== hashFile(source)
    ) {
      fs.copyFileSync(source, destination)
    }
  }
}

async function provision({
  name,
  url,
  sha256,
  archivePath,
  destination,
  marker,
}) {
  if (isProvisioned(destination, marker, sha256)) return

  if (!fs.existsSync(archivePath) || hashFile(archivePath) !== sha256) {
    fs.rmSync(archivePath, {force: true})
    process.stderr.write(`Downloading pinned ${name}\n`)
    await download(url, archivePath)
  }
  const actualHash = hashFile(archivePath)
  if (actualHash !== sha256) {
    fs.rmSync(archivePath, {force: true})
    throw new Error(
      `${name} SHA-256 mismatch: expected ${sha256}, received ${actualHash}`,
    )
  }

  const staging = `${destination}.extracting-${process.pid}`
  fs.rmSync(staging, {recursive: true, force: true})
  fs.mkdirSync(staging, {recursive: true})
  const result = spawnSync('tar', ['-xf', archivePath, '-C', staging], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  if (result.error || result.status !== 0) {
    fs.rmSync(staging, {recursive: true, force: true})
    throw new Error(
      `Unable to extract ${name}: ${result.error?.message ?? result.stderr}`,
    )
  }

  const entries = fs.readdirSync(staging, {withFileTypes: true})
  const directories = entries.filter((entry) => entry.isDirectory())
  if (entries.length !== 1 || directories.length !== 1) {
    fs.rmSync(staging, {recursive: true, force: true})
    throw new Error(`${name} archive did not contain one root directory`)
  }

  const extractedRoot = path.join(staging, directories[0].name)
  fs.rmSync(destination, {recursive: true, force: true})
  fs.renameSync(extractedRoot, destination)
  fs.rmSync(staging, {recursive: true, force: true})
  fs.writeFileSync(
    path.join(destination, '.lumine-provisioned.json'),
    `${JSON.stringify({name, url, sha256}, null, 2)}\n`,
  )

  if (!isProvisioned(destination, marker, sha256)) {
    throw new Error(`${name} was extracted without ${marker}`)
  }
}

function isProvisioned(destination, marker, sha256) {
  const stampPath = path.join(destination, '.lumine-provisioned.json')
  if (
    !fs.existsSync(path.join(destination, marker)) ||
    !fs.existsSync(stampPath)
  ) {
    return false
  }
  try {
    return JSON.parse(fs.readFileSync(stampPath, 'utf8')).sha256 === sha256
  } catch {
    return false
  }
}

async function download(url, destination) {
  const temporary = `${destination}.downloading-${process.pid}`
  fs.rmSync(temporary, {force: true})
  const response = await fetch(url, {redirect: 'follow'})
  if (!response.ok)
    throw new Error(`Download failed (${response.status}) for ${url}`)
  const bytes = Buffer.from(await response.arrayBuffer())
  fs.writeFileSync(temporary, bytes)
  fs.renameSync(temporary, destination)
}

function hashFile(filePath) {
  return crypto
    .createHash('sha256')
    .update(fs.readFileSync(filePath))
    .digest('hex')
}

main().catch((error) => {
  process.stderr.write(`${error.stack ?? error}\n`)
  process.exitCode = 1
})
