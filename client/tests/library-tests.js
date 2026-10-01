const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');
const Module = require('node:module');
const filename = path.resolve(__dirname, '../app/utils/library.ts');
const source = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {compilerOptions: {module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2022}}).outputText;
const moduleInstance = new Module(filename); moduleInstance._compile(source, filename);
const {filterLibrary, completedFiles} = moduleInstance.exports;
const tracks = [
  {id:'a',title:'Canción',artist:'Ana',album:'Show',format:'wav',status:'available',duration_ms:12000},
  {id:'b',title:'B',artist:'Luis',album:'Sport',format:'mp3',status:'missing',duration_ms:900},
  {id:'c',title:'A',artist:'ANA',album:'Show',format:'wav',status:'corrupt',duration_ms:4000}
];
assert.deepEqual(filterLibrary(tracks,'cancion','','','title').map(t=>t.id),['a']);
assert.deepEqual(filterLibrary(tracks,'ana','wav','','duration_ms').map(t=>t.id),['c','a']);
assert.deepEqual(filterLibrary(tracks,'','','missing','title').map(t=>t.id),['b']);
assert.deepEqual(filterLibrary(tracks,'','','','duration_ms',true).map(t=>t.id),['a','c','b']);
assert.deepEqual(tracks.map(t=>t.id),['a','b','c']);
assert.equal(completedFiles({files:[{status:'queued'},{status:'failed'},{status:'cancelled'},{status:'duplicate'},{status:'imported'}]}),4);
console.log('PASS Library search, numeric sorting, format/state filtering and partial batch progress');
