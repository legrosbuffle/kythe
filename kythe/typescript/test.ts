/*
 * Copyright 2017 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This program runs the Kythe verifier on test cases in the testdata/
// directory.  It's written in TypeScript (rather than a plain shell
// script) so it can reuse TypeScript data structures across test cases,
// speeding up the test.
//
// If run with no arguments, runs all tests found in testdata/.
// Otherwise run any files through the verifier by passing them:
//   node test.js path/to/test1.ts testdata/test2.ts

import * as assert from 'assert';
import * as child_process from 'child_process';
import * as path from 'path';
import * as ts from 'typescript';

import * as indexer from './indexer';

const KYTHE_PATH = '/opt/kythe';

/**
 * createTestCompilerHost creates a ts.CompilerHost that caches its default
 * library.  This prevents re-parsing the (big) TypeScript standard library
 * across each test.
 */
function createTestCompilerHost(options: ts.CompilerOptions): ts.CompilerHost {
  let compilerHost = ts.createCompilerHost(options);

  let libPath = compilerHost.getDefaultLibFileName(options);
  let libSource = compilerHost.getSourceFile(libPath, ts.ScriptTarget.ES2015);

  let hostGetSourceFile = compilerHost.getSourceFile;
  compilerHost.getSourceFile =
      (fileName: string, languageVersion: ts.ScriptTarget,
       onError?: (message: string) => void): ts.SourceFile => {
        if (fileName === libPath) return libSource;
        return hostGetSourceFile(fileName, languageVersion, onError);
      };
  return compilerHost;
}

/**
 * verify runs the indexer against a test case and passes it through the
 * Kythe verifier.  It returns a Promise because the node subprocess API must
 * be run async; if there's an error, it will reject the promise.
 */
function verify(
    host: ts.CompilerHost, options: ts.CompilerOptions,
    test: string): Promise<void> {
  let program = ts.createProgram([test], options, host);

  let verifier = child_process.spawn(
      `${KYTHE_PATH}/tools/entrystream --read_json |` +
          `${KYTHE_PATH}/tools/verifier ${test}`,
      [], {
        stdio: ['pipe', process.stdout, process.stderr],
        shell: true,
      });

  indexer.index('testcorpus', [test], program, (obj: {}) => {
    verifier.stdin.write(JSON.stringify(obj) + '\n');
  });
  verifier.stdin.end();

  return new Promise<void>((resolve, reject) => {
    verifier.on('close', (exitCode) => {
      if (exitCode === 0) {
        resolve();
      } else {
        reject(`process exited with code ${exitCode}`);
      }
    });
  });
}

function testLoadTsConfig() {
  let config = indexer.loadTsConfig('testdata/tsconfig-files.json', 'testdata');
  // We expect the paths that were loaded to be absolute.
  assert.deepEqual(config.fileNames, [path.resolve('testdata/alt.ts')]);
}

async function testIndexer(args: string[]) {
  let config = indexer.loadTsConfig('testdata/tsconfig.json', 'testdata');
  let testPaths = args.map(path.resolve);
  if (args.length === 0) {
    // If no tests were passed on the command line, run all the .ts files found
    // by the tsconfig.json, which covers all the tests in testdata/.
    testPaths = config.fileNames;
  }

  let host = createTestCompilerHost(config.options);
  for (const test of testPaths) {
    const testName = path.relative(config.options.rootDir!, test);
    let start = new Date().valueOf();
    process.stdout.write(`${testName}: `);
    try {
      await verify(host, config.options, test);
    } catch (e) {
      console.log('FAIL');
      throw e;
    }
    let time = new Date().valueOf() - start;
    console.log('PASS', time + 'ms');
  }
  return 0;
}

async function testMain(args: string[]) {
  testLoadTsConfig();
  await testIndexer(args);
}

testMain(process.argv.slice(2))
    .then(() => {
      process.exitCode = 0;
    })
    .catch((e) => {
      console.error(e);
      process.exitCode = 1;
    });
