'use strict'

const fs = require('node:fs')
const path = require('node:path')

const packageRoot = path.resolve(__dirname, '..')
const buildRoot = path.join(packageRoot, 'build')
const releaseRoot = path.join(buildRoot, 'Release')
const addonPath = path.join(releaseRoot, 'document-engine.node')
const savedAddon = path.join(packageRoot, '.document-engine.node.installing')
const dependencyRoot = path.join(packageRoot, '.native-deps')

function assertPackageChild(target) {
  const relative = path.relative(packageRoot, path.resolve(target))
  if (!relative || relative.startsWith('..') || path.isAbsolute(relative)) {
    throw new Error(`Refusing to clean outside the package: ${target}`)
  }
}

for (const target of [
  buildRoot,
  releaseRoot,
  addonPath,
  savedAddon,
  dependencyRoot,
]) {
  assertPackageChild(target)
}

if (!fs.existsSync(addonPath)) {
  throw new Error(`Native build did not produce ${addonPath}`)
}

fs.rmSync(savedAddon, {force: true})
fs.copyFileSync(addonPath, savedAddon)
fs.rmSync(buildRoot, {recursive: true, force: true})
fs.mkdirSync(releaseRoot, {recursive: true})
fs.renameSync(savedAddon, addonPath)
fs.rmSync(dependencyRoot, {recursive: true, force: true})
