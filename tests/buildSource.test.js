'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const repoRoot = path.resolve(__dirname, '..');
const workerCmake = path.join(repoRoot, 'backend/inference-worker/CMakeLists.txt');
const cmake = fs.readFileSync(workerCmake, 'utf8');

test('the worker builds llama from the vendored copy inside backend/', () => {
  assert.match(
    cmake,
    /set\(EDGE_LLAMA_SRC_DIR\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}\/llama-src"\)/,
    'EDGE_LLAMA_SRC_DIR must point at backend/inference-worker/llama-src'
  );
  assert.match(
    cmake,
    /add_subdirectory\("\$\{EDGE_LLAMA_SRC_DIR\}"/,
    'the llama sources must be added from EDGE_LLAMA_SRC_DIR, not from anywhere else'
  );
  assert.ok(
    fs.existsSync(path.join(repoRoot, 'backend/inference-worker/llama-src/CMakeLists.txt')),
    'the vendored tree that variable names must actually exist'
  );
});

test('nothing in the build reaches outside backend/ for llama sources', () => {
  assert.ok(
    !/main-llama\.cpp/.test(cmake),
    'the build must never reference the reference tree at main-llama.cpp'
  );
  for (const escape of ['../../main-llama', '../../../']) {
    assert.ok(!cmake.includes(escape), `stale escaping path in the build: ${escape}`);
  }
});

test('llama is linked from the in-tree target, never from an installed one', () => {
  // Either one would silently link a system llama nobody vendored.
  assert.ok(!/find_package\s*\(\s*llama/i.test(cmake), 'no find_package(llama)');
  assert.ok(!/find_library\s*\(\s*[^)]*llama/i.test(cmake), 'no find_library for llama');
  assert.ok(
    !/FetchContent|ExternalProject_Add/.test(cmake),
    'no downloaded llama: the vendored copy is the only source'
  );
  assert.match(cmake, /target_link_libraries\(edge-inference-worker PRIVATE llama\)/);
  assert.match(cmake, /if\(TARGET llama\)/);
});

test('every include path the worker gets for llama lives under the vendored tree', () => {
  const includes = cmake.match(/"\$\{EDGE_LLAMA_SRC_DIR\}\/[^"]+"/g) || [];
  assert.ok(includes.length >= 2, 'the worker needs llama and ggml headers');
  for (const include of includes) {
    assert.ok(
      include.startsWith('"${EDGE_LLAMA_SRC_DIR}/'),
      `include path escapes the vendored tree: ${include}`
    );
  }
});

test('no source path in the build points at a file that is not there', () => {
  const sources = cmake.match(/^\s{2}([./A-Za-z0-9_-]+\.cpp)$/gm) || [];
  assert.ok(sources.length > 0, 'the worker target must list sources');
  const workerDir = path.dirname(workerCmake);
  for (const raw of sources) {
    const rel = raw.trim();
    assert.ok(
      fs.existsSync(path.resolve(workerDir, rel)),
      `stale source path in the worker target: ${rel}`
    );
  }
});

test('the vendored tree keeps the upstream licence its own build reads', () => {
  // Removing it breaks upstream's CMakeLists and breaks MIT redistribution.
  assert.ok(
    fs.existsSync(path.join(repoRoot, 'backend/inference-worker/llama-src/LICENSE')),
    'llama-src/LICENSE must ship with the vendored source'
  );
});
