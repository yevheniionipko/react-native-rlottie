module.exports = {
  root: true,
  parser: '@typescript-eslint/parser',
  plugins: ['@typescript-eslint'],
  extends: [
    'eslint:recommended',
    'plugin:@typescript-eslint/recommended',
    'prettier',
  ],
  parserOptions: {
    ecmaVersion: 2021,
    sourceType: 'module',
    ecmaFeatures: {jsx: true},
  },
  env: {
    es2021: true,
    node: true,
  },
  ignorePatterns: [
    'lib/',
    'node_modules/',
    'example/',
    'cpp/',
    'ios/',
    'android/',
  ],
};
