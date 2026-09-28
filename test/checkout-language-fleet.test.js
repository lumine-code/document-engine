'use strict'

const assert = require('node:assert/strict')
const test = require('node:test')
const {catalogRepositories} = require('../script/checkout-language-fleet')

test('selects unique language repositories at immutable catalog SHAs', () => {
  const sha = 'a'.repeat(40)
  const repositories = catalogRepositories([
    {source: 'lumine-code/about', resolvedSha: sha, metadata: {name: 'about'}},
    {
      source: 'lumine-code/language-js',
      resolvedSha: sha,
      metadata: {name: 'language-js'},
    },
    {
      source: 'lumine-code/language-js',
      resolvedSha: sha,
      metadata: {name: 'language-js'},
    },
    {
      source: 'lumine-code/language-css',
      resolvedSha: 'b'.repeat(40),
      metadata: {name: 'language-css'},
    },
  ])
  assert.deepEqual(repositories, [
    {
      name: 'language-css',
      sha: 'b'.repeat(40),
      source: 'lumine-code/language-css',
    },
    {name: 'language-js', sha, source: 'lumine-code/language-js'},
  ])
})

test('rejects moving or external language catalog entries', () => {
  assert.throws(
    () =>
      catalogRepositories([
        {
          source: 'other/language-js',
          resolvedSha: 'main',
          metadata: {name: 'language-js'},
        },
      ]),
    /immutable Lumine source/,
  )
})
