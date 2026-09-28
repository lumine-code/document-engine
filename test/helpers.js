'use strict'

const path = require('node:path')

function loadSuperstring() {
  const configured = process.env.SUPERSTRING_PATH
  const target = configured
    ? path.resolve(configured)
    : path.resolve(__dirname, '..', '..', 'superstring')
  return require(target)
}

module.exports = {loadSuperstring}
