'use strict'

let binding
try {
  binding = require('../build/Release/document-engine.node')
} catch (releaseError) {
  try {
    binding = require('../build/Debug/document-engine.node')
  } catch {
    throw releaseError
  }
}

module.exports = binding
