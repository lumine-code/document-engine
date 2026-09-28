const js = require('@eslint/js')
const prettier = require('eslint-config-prettier')
const globals = require('globals')

module.exports = [
  {
    ignores: ['.native-deps/**', 'build/**', 'node_modules/**'],
  },
  js.configs.recommended,
  {
    files: ['**/*.js'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'commonjs',
      globals: globals.node,
    },
    rules: {
      'no-unused-vars': ['error', {argsIgnorePattern: '^_'}],
    },
  },
  prettier,
]
