// saQut TextMate grameri scope testi (ADR-045 threading sözdizimi dahil).
//
// examples/threading/*.sqt dosyalarını ve küçük satır örneklerini gramere
// karşı tokenize eder; beklenen token -> scope eşleşmelerini doğrular.
// Çalıştırma: npm run test:grammar   (editor/vscode içinde)
'use strict';

const fs = require('fs');
const path = require('path');
const vsctm = require('vscode-textmate');
const oniguruma = require('vscode-oniguruma');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const GRAMMAR = path.resolve(__dirname, '..', 'syntaxes', 'sqt.tmLanguage.json');

const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
const onigLib = oniguruma.loadWASM(wasm).then(() => ({
  createOnigScanner: (p) => new oniguruma.OnigScanner(p),
  createOnigString: (s) => new oniguruma.OnigString(s),
}));

const registry = new vsctm.Registry({
  onigLib,
  loadGrammar: async (scopeName) =>
    scopeName === 'source.sqt'
      ? vsctm.parseRawGrammar(fs.readFileSync(GRAMMAR, 'utf8'), GRAMMAR)
      : null,
});

function tokenize(grammar, text) {
  const out = [];
  let state = vsctm.INITIAL;
  for (const line of text.split('\n')) {
    const r = grammar.tokenizeLine(line, state);
    for (const t of r.tokens)
      out.push({ text: line.substring(t.startIndex, t.endIndex), scopes: t.scopes });
    state = r.ruleStack;
  }
  return out;
}

let failures = 0;
function expectScope(tokens, text, scope, label) {
  const hits = tokens.filter((t) => t.text.trim() === text);
  if (hits.length === 0) {
    console.error(`FAIL ${label}: '${text}' token'ı bulunamadı`);
    failures++;
    return;
  }
  for (const h of hits) {
    if (!h.scopes.includes(scope)) {
      console.error(`FAIL ${label}: '${text}' -> [${h.scopes.join(', ')}], beklenen ${scope}`);
      failures++;
      return;
    }
  }
}

(async () => {
  const grammar = await registry.loadGrammar('source.sqt');

  // 1) Satır örnekleri — her yeni sözdizimi öğesi.
  const cases = [
    ['shared int total = 0;', [['shared', 'storage.modifier.shared.sqt'], ['int', 'storage.type.sqt']]],
    ['shared Pool jobs = Pool(int);', [['Pool', 'storage.type.sqt'], ['int', 'storage.type.sqt']]],
    ['shared List seen = List(Job);', [['List', 'storage.type.sqt'], ['Job', 'entity.name.type.sqt']]],
    ['shared List rows = List(string[]);', [['string', 'storage.type.sqt'], ['[]', 'storage.type.sqt']]],
    ['Thread t = thread { work(); };', [['Thread', 'storage.type.sqt'], ['thread', 'keyword.control.thread.sqt'], ['work', 'entity.name.function.sqt']]],
    ['Thread[] ts = [];', [['Thread[]', 'storage.type.sqt']]],
    ['lock a, b;', [['lock', 'keyword.control.thread.sqt']]],
    ['unlock a;', [['unlock', 'keyword.control.thread.sqt']]],
    ['wait(done >= 3);', [['wait', 'keyword.control.thread.sqt']]],
    ['jobs.push(1); t.join(); t.stop();', [['push', 'entity.name.function.sqt'], ['join', 'entity.name.function.sqt']]],
    // Gerileme: mevcut sözdizimi değişmedi.
    ['int x = 5; // yorum', [['int', 'storage.type.sqt'], ['// yorum', 'comment.line.double-slash.sqt']]],
    ['while (true) { break; }', [['while', 'keyword.control.sqt'], ['true', 'constant.language.sqt']]],
    ['struct Job { int id; }', [['struct', 'keyword.other.sqt'], ['Job', 'entity.name.type.sqt']]],
  ];
  for (const [line, expects] of cases) {
    const toks = tokenize(grammar, line);
    for (const [text, scope] of expects) expectScope(toks, text, scope, JSON.stringify(line));
  }

  // 2) examples/threading/*.sqt — anahtar kelimeler hiçbir dosyada yanlış
  //    scope almamalı (ör. wait( fonksiyon çağrısı sanılmamalı).
  const dir = path.join(ROOT, 'examples', 'threading');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.sqt'));
  if (files.length === 0) {
    console.error('FAIL: examples/threading altında .sqt yok');
    failures++;
  }
  const kw = {
    shared: 'storage.modifier.shared.sqt',
    thread: 'keyword.control.thread.sqt',
    lock: 'keyword.control.thread.sqt',
    unlock: 'keyword.control.thread.sqt',
    wait: 'keyword.control.thread.sqt',
    Pool: 'storage.type.sqt',
    List: 'storage.type.sqt',
    Thread: 'storage.type.sqt',
  };
  for (const f of files) {
    const toks = tokenize(grammar, fs.readFileSync(path.join(dir, f), 'utf8'));
    for (const t of toks) {
      const word = t.text.trim();
      if (!(word in kw)) continue;
      if (t.scopes.some((s) => s.startsWith('comment.') || s.startsWith('string.'))) continue;
      if (!t.scopes.includes(kw[word])) {
        console.error(`FAIL ${f}: '${word}' -> [${t.scopes.join(', ')}], beklenen ${kw[word]}`);
        failures++;
      }
    }
  }

  if (failures) {
    console.error(`grammar.test: ${failures} hata`);
    process.exit(1);
  }
  console.log(`grammar.test: OK (${cases.length} satır örneği, ${files.length} örnek dosya)`);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
