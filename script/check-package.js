'use strict'

const fs = require('node:fs')
const path = require('node:path')
const {spawnSync} = require('node:child_process')

const packageRoot = path.resolve(__dirname, '..')
const manifest = require('../package.json')
const nativeDependencies = require('./native-dependencies.json')

function fail(message) {
  throw new Error(message)
}

function checkDescription() {
  const readme = fs.readFileSync(path.join(packageRoot, 'README.md'), 'utf8')
  const lines = readme.split(/\r?\n/)
  if (lines[0] !== '# document-engine') fail('README heading is not canonical')
  const firstProse = lines.slice(1).find((line) => line.trim() !== '')
  if (firstProse !== manifest.description) {
    fail('README and package.json descriptions differ')
  }
  if (!manifest.description.endsWith('.') || manifest.description.length > 80) {
    fail('Canonical description must be one terse sentence ending in a period')
  }
  const sections = ['## Features', '## Installation', '## Contributing']
  const sectionOffsets = sections.map((section) => lines.indexOf(section))
  if (
    sectionOffsets.some((offset) => offset < 0) ||
    !sectionOffsets.every(
      (offset, index) => index === 0 || sectionOffsets[index - 1] < offset,
    )
  ) {
    fail(
      'README must order Features, Installation and Contributing after the description',
    )
  }
  const featureLines = lines
    .slice(sectionOffsets[0] + 1, sectionOffsets[1])
    .filter((line) => line.startsWith('- '))
  if (
    featureLines.length < 3 ||
    featureLines.length > 9 ||
    featureLines.some((line) => !/^- \*\*[^*]+\*\*: [a-z].*\.$/.test(line))
  ) {
    fail(
      'README Features must contain 3–9 labelled one-line bullets ending in periods',
    )
  }
  const contributing = lines
    .slice(sectionOffsets[2] + 1)
    .find((line) => line.trim() !== '')
  if (
    contributing !==
    'Got ideas to make this package better, found a bug, or want to help add new features? Just drop your thoughts on GitHub. Any feedback is welcome!'
  ) {
    fail('README Contributing text is not canonical')
  }
}

function checkNativeManifest() {
  const sha = /^[0-9a-f]{64}$/
  for (const [name, dependency] of Object.entries({
    treeSitter: nativeDependencies.treeSitter,
    pcre2: nativeDependencies.pcre2,
  })) {
    if (
      !dependency.version ||
      !dependency.url.startsWith('https://github.com/')
    ) {
      fail(`${name} source dependency is not pinned to GitHub HTTPS`)
    }
    if (!sha.test(dependency.sha256)) fail(`${name} SHA-256 is invalid`)
  }
  const requiredPlatforms = [
    'win32-x64',
    'win32-arm64',
    'linux-x64',
    'linux-arm64',
    'darwin-x64',
    'darwin-arm64',
  ]
  for (const platform of requiredPlatforms) {
    const artifact = nativeDependencies.wasmtime.artifacts[platform]
    if (!artifact?.file || !sha.test(artifact.sha256)) {
      fail(`Wasmtime artifact is missing or unpinned for ${platform}`)
    }
  }
  for (const license of ['TREE-SITTER.txt', 'PCRE2.txt', 'WASMTIME.txt']) {
    if (!fs.existsSync(path.join(packageRoot, 'licenses', license))) {
      fail(`Missing bundled license: ${license}`)
    }
  }
}

function checkManifest() {
  if (manifest.name !== '@lumine-code/document-engine')
    fail('Package name changed')
  if (manifest.author !== 'lumine-code')
    fail('Package author must be lumine-code')
  if (manifest.version !== '1.0.0')
    fail('The first ecosystem release must be 1.0.0')
  if (
    manifest.repository !== 'https://github.com/lumine-code/document-engine'
  ) {
    fail('Repository URL is not canonical')
  }
  if (
    manifest.bugs !== 'https://github.com/lumine-code/document-engine/issues'
  ) {
    fail('Bug tracker URL is not canonical')
  }
  if (manifest.private !== true || manifest.publishConfig != null) {
    fail('document-engine must be blocked from npm publication')
  }
  if (manifest.engines?.node !== '>=24')
    fail('Node 24 runtime floor is required')
  if (manifest.engines?.lumine) fail('document-engine must remain a library')
  if (!manifest.dependencies?.['node-addon-api'])
    fail('node-addon-api is required')
  if (
    !Array.isArray(manifest.keywords) ||
    manifest.keywords.length < 3 ||
    manifest.keywords.length > 8
  ) {
    fail('Libraries require 3–8 accurate keywords')
  }
  if (
    !/provision-native-deps\.js.+node-gyp rebuild.+cleanup-install-artifacts\.js/.test(
      manifest.scripts?.install ?? '',
    )
  )
    fail('install must provision, build and clean in that order')
  for (const required of [
    'binding.gyp',
    'lib',
    'src',
    'script',
    'README.md',
    'THIRD_PARTY_NOTICES.md',
    'licenses',
    'LICENSE',
  ]) {
    if (!manifest.files?.includes(required)) fail(`files omits ${required}`)
  }
  if (process.argv.includes('--release')) {
    const superstring = manifest.dependencies?.['@lumine-code/superstring']
    if (
      !/^github:lumine-code\/superstring#[0-9a-f]{40}$/.test(superstring ?? '')
    ) {
      fail(
        'Release requires an exact github:lumine-code/superstring#<40-char-sha> dependency',
      )
    }
  }
}

function checkBindingContract() {
  const binding = fs.readFileSync(path.join(packageRoot, 'binding.gyp'), 'utf8')
  for (const required of [
    'NAPI_VERSION=<(napi_build_version)',
    'NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS',
    'PCRE2_STATIC',
    'TREE_SITTER_FEATURE_WASM',
    'resolve-superstring-abi.js --print-include',
  ]) {
    if (!binding.includes(required)) fail(`binding.gyp omits ${required}`)
  }
  for (const library of [
    '<(wasmtime_root)/lib/wasmtime.lib',
    '<(wasmtime_root)/lib/libwasmtime.a',
  ]) {
    if (!binding.includes(library)) fail(`binding.gyp omits ${library}`)
  }
}

function checkPack() {
  const npm = process.platform === 'win32' ? 'cmd.exe' : 'npm'
  const args =
    process.platform === 'win32'
      ? ['/d', '/s', '/c', 'npm pack --dry-run --json --ignore-scripts']
      : ['pack', '--dry-run', '--json', '--ignore-scripts']
  const result = spawnSync(npm, args, {
    cwd: packageRoot,
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  if (result.error || result.status !== 0) {
    fail(`npm pack --dry-run failed: ${result.error?.message ?? result.stderr}`)
  }
  const [pack] = JSON.parse(result.stdout)
  const paths = new Set(
    pack.files.map((file) => file.path.replaceAll('\\', '/')),
  )
  for (const required of [
    'binding.gyp',
    'lib/main.js',
    'lib/main.d.ts',
    'docs/packaging.md',
    'script/provision-native-deps.js',
    'script/native-dependencies.json',
    'script/cleanup-install-artifacts.js',
    'script/resolve-superstring-abi.js',
    'src/bindings/addon.cc',
    'licenses/TREE-SITTER.txt',
    'licenses/PCRE2.txt',
    'licenses/WASMTIME.txt',
  ]) {
    if (!paths.has(required)) fail(`Packed archive omits ${required}`)
  }
  for (const filePath of paths) {
    if (
      /^(?:test|build|node_modules|\.native-deps|\.github)\//.test(filePath)
    ) {
      fail(`Packed archive contains forbidden path: ${filePath}`)
    }
  }
  if (pack.size > 1024 * 1024 || pack.unpackedSize > 4 * 1024 * 1024) {
    fail(
      `Source package is unexpectedly large: ${pack.size}/${pack.unpackedSize}`,
    )
  }
  return {
    entryCount: pack.entryCount,
    packedBytes: pack.size,
    unpackedBytes: pack.unpackedSize,
  }
}

checkDescription()
checkNativeManifest()
checkManifest()
checkBindingContract()
const pack = checkPack()
const abi = spawnSync(
  process.execPath,
  [path.join(__dirname, 'resolve-superstring-abi.js'), '--check'],
  {cwd: packageRoot, encoding: 'utf8'},
)
if (abi.error || abi.status !== 0) {
  fail(`SnapshotLease check failed: ${abi.error?.message ?? abi.stderr}`)
}
process.stdout.write(
  `${JSON.stringify(
    {
      package: manifest.name,
      version: manifest.version,
      ...pack,
      snapshotLease: JSON.parse(abi.stdout),
    },
    null,
    2,
  )}\n`,
)
