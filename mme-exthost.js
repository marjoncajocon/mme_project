// mme-exthost.js - mme's extension host: VS Code extensions' code, run by Node
//
// mme (C) starts this with node and talks to it as to a language server
// (LSP over stdio, Content-Length framing). It loads the extensions named in
// mme.extensions.run, gives each `require('vscode')` an API of its own making,
// and turns mme's LSP requests into calls on the providers the extensions
// register (languages.register*Provider) - their answers go back as LSP. What
// LSP has no word for (a quick pick, an output channel, a status bar item)
// goes as mme/* messages. Anything of the vscode API not made yet is a no-op
// that says so once in Output > Extension Host.
//
//   node mme-exthost.js                   the host (mme runs it)
//   node mme-exthost.js --emit-header     ehost_js.h, this file as C strings for ehost.c
'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const Module = require('module');
const cp = require('child_process');
const crypto = require('crypto');

if (process.argv[2] === '--emit-header') {	// the file as C string literals, a line each
  const src = fs.readFileSync(__filename, 'utf8').replace(/\r\n/g, '\n').split('\n');
  if (src[src.length - 1] === '') src.pop();
  const out = ['/* ehost_js.h - mme-exthost.js (ehost.c writes it into mme-data), a line each: made from it by',
    '** node mme-exthost.js --emit-header, not edited here */'];
  for (const l of src) out.push('  "' + l.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\t/g, '\\t') + '\\n",');
  process.stdout.write(out.join('\n') + '\n');
  process.exit(0);
}


// ----------------------------------------------------------------- the wire

const stdout = process.stdout.write.bind(process.stdout);
let inBuf = Buffer.alloc(0);
let nextId = 1;
const pending = new Map();	// our requests to mme: id -> {res, rej}
const cancels = new Map();	// mme's requests to us: id -> CancellationTokenSource

function send (msg) {
  const s = JSON.stringify(msg);
  stdout('Content-Length: ' + Buffer.byteLength(s, 'utf8') + '\r\n\r\n' + s);
}

function request (method, params) {
  return new Promise((res, rej) => {
    const id = 'h' + nextId++;
    pending.set(id, {res, rej});
    send({jsonrpc: '2.0', id, method, params});
  });
}

function notify (method, params) {
  send({jsonrpc: '2.0', method, params});
}

process.stdin.on('data', (d) => {
  inBuf = Buffer.concat([inBuf, d]);
  for (;;) {
    const sep = inBuf.indexOf('\r\n\r\n');
    if (sep < 0) return;
    const m = /Content-Length:\s*(\d+)/i.exec(inBuf.slice(0, sep).toString('ascii'));
    if (!m) {
      inBuf = inBuf.slice(sep + 4);
      continue;
    }
    const n = +m[1];
    if (inBuf.length < sep + 4 + n) return;
    const body = inBuf.slice(sep + 4, sep + 4 + n).toString('utf8');
    inBuf = inBuf.slice(sep + 4 + n);
    let msg;
    try {
      msg = JSON.parse(body);
    } catch (e) {
      log('[error] a message that is not JSON: ' + e.message);
      continue;
    }
    dispatch(msg);
  }
});
process.stdin.on('end', () => process.exit(0));


// ----------------------------------------------------------------- the log

// Output > Extension Host. console.* and stray writes to stdout land here too:
// stdout is the wire to mme and must carry nothing else.
function log (s) {
  notify('mme/output', {channel: 'Extension Host', text: s + '\n'});
}

function fmt (args) {
  return args.map((a) => (typeof a === 'string' ? a : a instanceof Error ? (a.stack || a.message) : safeJson(a))).join(' ');
}

function safeJson (v) {
  try {
    return JSON.stringify(v);
  } catch (e) {
    return String(v);
  }
}

console.log = console.info = console.debug = (...a) => log(fmt(a));
console.warn = (...a) => log('[warning] ' + fmt(a));
console.error = (...a) => log('[error] ' + fmt(a));
process.stdout.write = (chunk, enc, cb) => {
  log(String(chunk).replace(/\n$/, ''));
  if (typeof enc === 'function') enc();
  else if (typeof cb === 'function') cb();
  return true;
};
process.noDeprecation = true;	// node's own warnings about old modules extensions use: not theirs to see
process.on('uncaughtException', (e) => log('[error] ' + (e && e.stack ? e.stack : e)));
process.on('unhandledRejection', (e) => log('[error] unhandled: ' + (e && e.stack ? e.stack : e)));

const missing = new Set();
function said (name) {	// an API not made yet: said once
  if (missing.has(name)) return;
  missing.add(name);
  log('[missing] ' + name);
}


// ----------------------------------------------------------------- the basic types

class Disposable {
  constructor (fn) {
    this._fn = fn;
  }
  static from (...ds) {
    return new Disposable(() => ds.forEach((d) => d && typeof d.dispose === 'function' && d.dispose()));
  }
  dispose () {
    if (this._fn) {
      const f = this._fn;
      this._fn = null;
      f();
    }
  }
}

class EventEmitter {
  constructor () {
    this._ls = [];
    this.event = (listener, thisArg, disposables) => {
      const l = {listener, thisArg};
      this._ls.push(l);
      const d = new Disposable(() => {
        const i = this._ls.indexOf(l);
        if (i >= 0) this._ls.splice(i, 1);
      });
      if (Array.isArray(disposables)) disposables.push(d);
      return d;
    };
  }
  fire (e) {
    for (const l of this._ls.slice()) {
      try {
        l.listener.call(l.thisArg, e);
      } catch (err) {
        log('[error] an event listener: ' + (err && err.stack ? err.stack : err));
      }
    }
  }
  dispose () {
    this._ls = [];
  }
}

class CancellationError extends Error {
  constructor () {
    super('Canceled');
    this.name = 'Canceled';
  }
}

class CancellationTokenSource {
  constructor () {
    const ev = new EventEmitter();
    this._ev = ev;
    this.token = {isCancellationRequested: false, onCancellationRequested: ev.event};
  }
  cancel () {
    if (!this.token.isCancellationRequested) {
      this.token.isCancellationRequested = true;
      this._ev.fire();
    }
  }
  dispose () {
    this._ev.dispose();
  }
}
const noToken = {isCancellationRequested: false, onCancellationRequested: () => new Disposable(() => {})};

class Position {
  constructor (line, character) {
    this.line = Math.max(0, line | 0);
    this.character = Math.max(0, character | 0);
  }
  isBefore (o) {
    return this.line < o.line || (this.line === o.line && this.character < o.character);
  }
  isBeforeOrEqual (o) {
    return this.isBefore(o) || this.isEqual(o);
  }
  isAfter (o) {
    return !this.isBeforeOrEqual(o);
  }
  isAfterOrEqual (o) {
    return !this.isBefore(o);
  }
  isEqual (o) {
    return this.line === o.line && this.character === o.character;
  }
  compareTo (o) {
    return this.isBefore(o) ? -1 : this.isEqual(o) ? 0 : 1;
  }
  translate (a, b) {
    if (typeof a === 'object' && a) return new Position(this.line + (a.lineDelta || 0), this.character + (a.characterDelta || 0));
    return new Position(this.line + (a || 0), this.character + (b || 0));
  }
  with (a, b) {
    if (typeof a === 'object' && a) return new Position(a.line !== undefined ? a.line : this.line, a.character !== undefined ? a.character : this.character);
    return new Position(a !== undefined ? a : this.line, b !== undefined ? b : this.character);
  }
  toJSON () {
    return {line: this.line, character: this.character};
  }
}

class Range {
  constructor (a, b, c, d) {
    let s, e;
    if (typeof a === 'number') {
      s = new Position(a, b);
      e = new Position(c, d);
    } else {
      s = new Position(a.line, a.character);
      e = new Position(b.line, b.character);
    }
    if (e.isBefore(s)) [s, e] = [e, s];
    this.start = s;
    this.end = e;
  }
  get isEmpty () {
    return this.start.isEqual(this.end);
  }
  get isSingleLine () {
    return this.start.line === this.end.line;
  }
  contains (x) {
    if (x instanceof Range || (x && x.start && x.end)) return this.contains(x.start) && this.contains(x.end);
    return !x.isBefore(this.start) && !this.end.isBefore(x);
  }
  isEqual (o) {
    return this.start.isEqual(o.start) && this.end.isEqual(o.end);
  }
  intersection (o) {
    const s = this.start.isAfter(o.start) ? this.start : o.start;
    const e = this.end.isBefore(o.end) ? this.end : o.end;
    return s.isAfter(e) ? undefined : new Range(s, e);
  }
  union (o) {
    return new Range(this.start.isBefore(o.start) ? this.start : o.start, this.end.isAfter(o.end) ? this.end : o.end);
  }
  with (a, b) {
    if (a && a.start === undefined && a.end === undefined && !(a instanceof Position) && typeof a === 'object')
      return new Range(this.start, this.end);
    if (a && (a.start || a.end) && !(a instanceof Position)) return new Range(a.start || this.start, a.end || this.end);
    return new Range(a || this.start, b || this.end);
  }
  toJSON () {
    return {start: this.start.toJSON(), end: this.end.toJSON()};
  }
}

class Selection extends Range {
  constructor (a, b, c, d) {
    let anchor, active;
    if (typeof a === 'number') {
      anchor = new Position(a, b);
      active = new Position(c, d);
    } else {
      anchor = a;
      active = b;
    }
    super(anchor, active);
    this.anchor = anchor;
    this.active = active;
  }
  get isReversed () {
    return this.active.isBefore(this.anchor);
  }
}

class Location {
  constructor (uri, rangeOrPosition) {
    this.uri = uri;
    this.range = rangeOrPosition instanceof Position ? new Range(rangeOrPosition, rangeOrPosition) : rangeOrPosition;
  }
}


// ----------------------------------------------------------------- Uri

const isWin = process.platform === 'win32';

function pctEncode (s, keepSlash) {
  let out = '';
  for (const ch of Buffer.from(s, 'utf8')) {
    const c = String.fromCharCode(ch);
    if (/[A-Za-z0-9\-._~]/.test(c) || (keepSlash && c === '/')) out += c;
    else out += '%' + ch.toString(16).toUpperCase().padStart(2, '0');
  }
  return out;
}

function pctDecode (s) {
  try {
    return decodeURIComponent(s);
  } catch (e) {
    return s;
  }
}

class Uri {
  constructor (scheme, authority, p, query, fragment) {
    this.scheme = scheme || 'file';
    this.authority = authority || '';
    this.path = p || '/';
    this.query = query || '';
    this.fragment = fragment || '';
  }
  static parse (s, strict) {
    const m = /^([a-zA-Z][\w+.-]*):(\/\/([^/?#]*))?([^?#]*)(\?([^#]*))?(#(.*))?$/.exec(s || '');
    if (!m) {
      if (strict) throw new Error('not a uri: ' + s);
      return new Uri('file', '', '/' + s);
    }
    let p = pctDecode(m[4] || '');
    if (m[1] === 'file' && !p.startsWith('/')) p = '/' + p;
    return new Uri(m[1], pctDecode(m[3] || ''), p, pctDecode(m[6] || ''), pctDecode(m[8] || ''));
  }
  static file (f) {
    let p = String(f);
    let auth = '';
    if (isWin) p = p.replace(/\\/g, '/');
    if (p.startsWith('//')) {	// a UNC path
      const i = p.indexOf('/', 2);
      auth = i < 0 ? p.slice(2) : p.slice(2, i);
      p = i < 0 ? '/' : p.slice(i);
    }
    if (!p.startsWith('/')) p = '/' + p;
    return new Uri('file', auth, p, '', '');
  }
  static joinPath (base, ...parts) {
    return base.with({path: path.posix.join(base.path, ...parts.map((x) => String(x).replace(/\\/g, '/')))});
  }
  static from (c) {
    return new Uri(c.scheme, c.authority, c.path, c.query, c.fragment);
  }
  static isUri (v) {
    return v instanceof Uri;
  }
  get fsPath () {
    let p = this.path;
    if (this.authority && this.scheme === 'file') p = '//' + this.authority + p;
    else if (/^\/[a-zA-Z]:/.test(p)) p = p[1].toLowerCase() + p.slice(2);
    return isWin ? p.replace(/\//g, '\\') : p;
  }
  with (c) {
    return new Uri(c.scheme !== undefined ? c.scheme : this.scheme, c.authority !== undefined ? c.authority : this.authority,
      c.path !== undefined ? c.path : this.path, c.query !== undefined ? c.query : this.query,
      c.fragment !== undefined ? c.fragment : this.fragment);
  }
  toString (skipEncoding) {
    const enc = skipEncoding ? (s) => s : (s, sl) => pctEncode(s, sl);
    let s = this.scheme + ':';
    if (this.authority || this.scheme === 'file') s += '//' + enc(this.authority, false);
    let p = this.path;
    const drive = /^\/([a-zA-Z]):(.*)$/.exec(p);
    if (drive) s += '/' + drive[1].toLowerCase() + (skipEncoding ? ':' : '%3A') + enc(drive[2], true);
    else s += enc(p, true);
    if (this.query) s += '?' + enc(this.query, false);
    if (this.fragment) s += '#' + enc(this.fragment, false);
    return s;
  }
  toJSON () {
    return {$mid: 1, scheme: this.scheme, authority: this.authority, path: this.path, query: this.query, fragment: this.fragment,
      fsPath: this.fsPath, external: this.toString()};
  }
}

// one key for a file, whichever way its uri was written (mme writes E:/, VS Code e%3A/)
function uriKey (u) {
  if (typeof u === 'string') u = Uri.parse(u);
  if (u.scheme !== 'file') return u.toString();
  const f = path.normalize(u.fsPath);
  return 'file:' + (isWin ? f.toLowerCase() : f);
}

// the uri string mme knows a file by (to_uri in elsp.c): file:///E:/a/b with a few escapes
const mmeUris = new Map();	// uriKey -> what mme sent
function mmeUri (u) {
  if (typeof u === 'string') u = Uri.parse(u);
  const known = mmeUris.get(uriKey(u));
  if (known) return known;
  if (u.scheme !== 'file') return u.toString();
  let p = u.fsPath;
  if (isWin) p = p.replace(/\\/g, '/');
  if (/^[a-z]:/.test(p)) p = p[0].toUpperCase() + p.slice(1);
  let s = 'file://' + (p.startsWith('/') ? '' : '/');
  for (const ch of Buffer.from(p, 'utf8')) {
    const c = String.fromCharCode(ch);
    if (ch < 32 || ch > 127 || ' "\\%#?[]'.includes(c)) s += '%' + ch.toString(16).toUpperCase().padStart(2, '0');
    else s += c;
  }
  return s;
}

function toUri (x) {
  if (x instanceof Uri) return x;
  if (typeof x === 'string') return /^[a-zA-Z][\w+.-]+:/.test(x) && !/^[a-zA-Z]:[\\/]/.test(x) ? Uri.parse(x) : Uri.file(x);
  if (x && x.scheme) return Uri.from(x);
  return Uri.file(String(x));
}


// ----------------------------------------------------------------- documents

const EndOfLine = {LF: 1, CRLF: 2};

class TextLine {
  constructor (line, text, hasNext) {
    this.lineNumber = line;
    this.text = text;
    this.range = new Range(line, 0, line, text.length);
    this.rangeIncludingLineBreak = hasNext ? new Range(line, 0, line + 1, 0) : this.range;
    this.firstNonWhitespaceCharacterIndex = text.search(/\S|$/);
    this.isEmptyOrWhitespace = this.firstNonWhitespaceCharacterIndex === text.length;
  }
}

class TextDocument {
  constructor (uri, languageId, version, text) {
    this.uri = uri;
    this.languageId = languageId;
    this.version = version;
    this.isClosed = false;
    this.isDirty = false;
    this.isUntitled = uri.scheme === 'untitled';
    this.encoding = 'utf8';
    this._set(text);
  }
  _set (text) {
    this._text = text;
    this._lines = null;
    this.eol = text.includes('\r\n') ? EndOfLine.CRLF : EndOfLine.LF;
  }
  get fileName () {
    return this.uri.fsPath;
  }
  get notebook () {
    return undefined;
  }
  _ls () {
    if (!this._lines) {
      this._lines = this._text.split(/\r\n|\r|\n/);
      this._starts = [];
      let off = 0;
      const re = /\r\n|\r|\n/g;
      this._starts.push(0);
      let m;
      while ((m = re.exec(this._text)) !== null) {
        off = m.index + m[0].length;
        this._starts.push(off);
      }
    }
    return this._lines;
  }
  get lineCount () {
    return this._ls().length;
  }
  getText (range) {
    if (!range) return this._text;
    const r = this.validateRange(range);
    return this._text.slice(this.offsetAt(r.start), this.offsetAt(r.end));
  }
  lineAt (x) {
    const ls = this._ls();
    const n = typeof x === 'number' ? x : x.line;
    if (n < 0 || n >= ls.length) throw new Error('Illegal value for `line`');
    return new TextLine(n, ls[n], n < ls.length - 1);
  }
  offsetAt (p) {
    const ls = this._ls();
    const pos = this.validatePosition(p);
    return this._starts[pos.line] + Math.min(pos.character, ls[pos.line].length);
  }
  positionAt (off) {
    this._ls();
    off = Math.max(0, Math.min(off | 0, this._text.length));
    let lo = 0;
    let hi = this._starts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (this._starts[mid] <= off) lo = mid;
      else hi = mid - 1;
    }
    return new Position(lo, Math.min(off - this._starts[lo], this._lines[lo].length));
  }
  validatePosition (p) {
    const ls = this._ls();
    let line = Math.min(Math.max(0, p.line), ls.length - 1);
    let ch = Math.min(Math.max(0, p.character), ls[line].length);
    if (p.line >= ls.length) ch = ls[line].length;
    return new Position(line, ch);
  }
  validateRange (r) {
    return new Range(this.validatePosition(r.start), this.validatePosition(r.end));
  }
  getWordRangeAtPosition (p, regex) {
    const line = this._ls()[p.line];
    if (line === undefined) return undefined;
    const re = regex ? new RegExp(regex.source, regex.flags.includes('g') ? regex.flags : regex.flags + 'g')
      : /(-?\d*\.\d\w*)|([^`~!@#$%^&*()\-=+[{\]}\\|;:'",.<>/?\s]+)/g;
    let m;
    while ((m = re.exec(line)) !== null) {
      if (m.index <= p.character && p.character <= m.index + m[0].length && m[0].length)
        return new Range(p.line, m.index, p.line, m.index + m[0].length);
      if (m[0].length === 0) re.lastIndex++;
    }
    return undefined;
  }
  save () {
    return request('mme/save', {uri: mmeUri(this.uri)}).then(() => true, () => false);
  }
}

const docs = new Map();	// uriKey -> TextDocument (open in mme)


// ----------------------------------------------------------------- edits, markdown, diagnostics, the feature types

class TextEdit {
  constructor (range, newText) {
    this.range = range;
    this.newText = newText;
  }
  static replace (r, t) {
    return new TextEdit(r, t);
  }
  static insert (p, t) {
    return new TextEdit(new Range(p, p), t);
  }
  static delete (r) {
    return new TextEdit(r, '');
  }
  static setEndOfLine (eol) {
    const e = new TextEdit(new Range(0, 0, 0, 0), '');
    e.newEol = eol;
    return e;
  }
}

class SnippetTextEdit {
  constructor (range, snippet) {
    this.range = range;
    this.snippet = snippet;
  }
  static replace (r, s) {
    return new SnippetTextEdit(r, s);
  }
  static insert (p, s) {
    return new SnippetTextEdit(new Range(p, p), s);
  }
}

class WorkspaceEdit {
  constructor () {
    this._edits = [];	// {uri, edit} | {op:'create'|'rename'|'delete', ...}
  }
  replace (uri, range, newText) {
    this._edits.push({uri, edit: new TextEdit(range, newText)});
  }
  insert (uri, pos, newText) {
    this.replace(uri, new Range(pos, pos), newText);
  }
  delete (uri, range) {
    this.replace(uri, range, '');
  }
  set (uri, edits) {
    this._edits = this._edits.filter((e) => !e.uri || uriKey(e.uri) !== uriKey(uri));
    for (const e of edits || []) {
      const te = Array.isArray(e) ? e[0] : e;
      if (te instanceof SnippetTextEdit) this.replace(uri, te.range, te.snippet.value);
      else if (te && te.range) this.replace(uri, te.range, te.newText);
    }
  }
  has (uri) {
    return this._edits.some((e) => e.uri && uriKey(e.uri) === uriKey(uri));
  }
  get (uri) {
    return this._edits.filter((e) => e.uri && uriKey(e.uri) === uriKey(uri)).map((e) => e.edit);
  }
  entries () {
    const m = new Map();
    for (const e of this._edits) {
      if (!e.uri) continue;
      const k = uriKey(e.uri);
      if (!m.has(k)) m.set(k, [e.uri, []]);
      m.get(k)[1].push(e.edit);
    }
    return [...m.values()];
  }
  createFile (uri, options) {
    this._edits.push({op: 'create', uri: undefined, target: uri, options: options || {}});
  }
  deleteFile (uri, options) {
    this._edits.push({op: 'delete', uri: undefined, target: uri, options: options || {}});
  }
  renameFile (from, to, options) {
    this._edits.push({op: 'rename', uri: undefined, target: from, to, options: options || {}});
  }
  get size () {
    return this.entries().length;
  }
}

class SnippetString {
  constructor (value) {
    this.value = value || '';
    this._n = 1;
  }
  static _esc (s) {
    return String(s).replace(/[$}\\]/g, '\\$&');
  }
  appendText (s) {
    this.value += SnippetString._esc(s);
    return this;
  }
  appendTabstop (n) {
    this.value += '$' + (n === undefined ? this._n++ : n);
    return this;
  }
  appendPlaceholder (v, n) {
    const k = n === undefined ? this._n++ : n;
    if (typeof v === 'function') {
      const inner = new SnippetString();
      inner._n = this._n;
      v(inner);
      this._n = inner._n;
      this.value += '${' + k + ':' + inner.value + '}';
    } else this.value += '${' + k + ':' + SnippetString._esc(v) + '}';
    return this;
  }
  appendChoice (vs, n) {
    this.value += '${' + (n === undefined ? this._n++ : n) + '|' + vs.map((v) => String(v).replace(/[,|\\]/g, '\\$&')).join(',') + '|}';
    return this;
  }
  appendVariable (name, def) {
    this.value += '${' + name + (def ? ':' + (typeof def === 'string' ? def : '') : '') + '}';
    return this;
  }
}

class MarkdownString {
  constructor (value, supportThemeIcons) {
    this.value = value || '';
    this.isTrusted = false;
    this.supportThemeIcons = !!supportThemeIcons;
    this.supportHtml = false;
  }
  appendText (s) {
    this.value += String(s).replace(/[\\`*_{}[\]()#+\-.!|<>]/g, '\\$&').replace(/\n/g, '\n\n');
    return this;
  }
  appendMarkdown (s) {
    this.value += s;
    return this;
  }
  appendCodeblock (code, lang) {
    this.value += '\n```' + (lang || '') + '\n' + code + '\n```\n';
    return this;
  }
}

const DiagnosticSeverity = {Error: 0, Warning: 1, Information: 2, Hint: 3};
const DiagnosticTag = {Unnecessary: 1, Deprecated: 2};

class Diagnostic {
  constructor (range, message, severity) {
    this.range = range;
    this.message = message;
    this.severity = severity === undefined ? DiagnosticSeverity.Error : severity;
  }
}

class DiagnosticRelatedInformation {
  constructor (location, message) {
    this.location = location;
    this.message = message;
  }
}

class Hover {
  constructor (contents, range) {
    this.contents = Array.isArray(contents) ? contents : [contents];
    this.range = range;
  }
}

class VerboseHover extends Hover {}

const CompletionItemKind = {Text: 0, Method: 1, Function: 2, Constructor: 3, Field: 4, Variable: 5, Class: 6, Interface: 7,
  Module: 8, Property: 9, Unit: 10, Value: 11, Enum: 12, Keyword: 13, Snippet: 14, Color: 15, File: 16, Reference: 17,
  Folder: 18, EnumMember: 19, Constant: 20, Struct: 21, Event: 22, Operator: 23, TypeParameter: 24, User: 25, Issue: 26};
const CompletionItemTag = {Deprecated: 1};
const CompletionTriggerKind = {Invoke: 0, TriggerCharacter: 1, TriggerForIncompleteCompletions: 2};

class CompletionItem {
  constructor (label, kind) {
    this.label = label;
    this.kind = kind;
  }
}

class CompletionList {
  constructor (items, isIncomplete) {
    this.items = items || [];
    this.isIncomplete = !!isIncomplete;
  }
}

class InlineCompletionItem {
  constructor (insertText, range, command) {
    this.insertText = insertText;
    this.range = range;
    this.command = command;
  }
}

class InlineCompletionList {
  constructor (items) {
    this.items = items;
  }
}

class SignatureHelp {
  constructor () {
    this.signatures = [];
    this.activeSignature = 0;
    this.activeParameter = 0;
  }
}

class SignatureInformation {
  constructor (label, documentation) {
    this.label = label;
    this.documentation = documentation;
    this.parameters = [];
  }
}

class ParameterInformation {
  constructor (label, documentation) {
    this.label = label;
    this.documentation = documentation;
  }
}

const SignatureHelpTriggerKind = {Invoke: 1, TriggerCharacter: 2, ContentChange: 3};
const DocumentHighlightKind = {Text: 0, Read: 1, Write: 2};

class DocumentHighlight {
  constructor (range, kind) {
    this.range = range;
    this.kind = kind === undefined ? DocumentHighlightKind.Text : kind;
  }
}

const SymbolKind = {File: 0, Module: 1, Namespace: 2, Package: 3, Class: 4, Method: 5, Property: 6, Field: 7, Constructor: 8,
  Enum: 9, Interface: 10, Function: 11, Variable: 12, Constant: 13, String: 14, Number: 15, Boolean: 16, Array: 17,
  Object: 18, Key: 19, Null: 20, EnumMember: 21, Struct: 22, Event: 23, Operator: 24, TypeParameter: 25};
const SymbolTag = {Deprecated: 1};

class SymbolInformation {
  constructor (name, kind, a, b, c) {
    this.name = name;
    this.kind = kind;
    if (a instanceof Location) {
      this.location = a;
      this.containerName = b;
    } else if (a instanceof Range) {
      this.location = new Location(b, a);
      this.containerName = c;
    } else {
      this.containerName = a;
      this.location = b;
    }
  }
}

class DocumentSymbol {
  constructor (name, detail, kind, range, selectionRange) {
    this.name = name;
    this.detail = detail;
    this.kind = kind;
    this.range = range;
    this.selectionRange = selectionRange;
    this.children = [];
  }
}

class CodeActionKind {
  constructor (value) {
    this.value = value;
  }
  append (p) {
    return new CodeActionKind(this.value ? this.value + '.' + p : p);
  }
  intersects (o) {
    return this.contains(o) || o.contains(this);
  }
  contains (o) {
    return this.value === '' || o.value === this.value || o.value.startsWith(this.value + '.');
  }
}
CodeActionKind.Empty = new CodeActionKind('');
CodeActionKind.QuickFix = new CodeActionKind('quickfix');
CodeActionKind.Refactor = new CodeActionKind('refactor');
CodeActionKind.RefactorExtract = new CodeActionKind('refactor.extract');
CodeActionKind.RefactorInline = new CodeActionKind('refactor.inline');
CodeActionKind.RefactorMove = new CodeActionKind('refactor.move');
CodeActionKind.RefactorRewrite = new CodeActionKind('refactor.rewrite');
CodeActionKind.Source = new CodeActionKind('source');
CodeActionKind.SourceOrganizeImports = new CodeActionKind('source.organizeImports');
CodeActionKind.SourceFixAll = new CodeActionKind('source.fixAll');
CodeActionKind.Notebook = new CodeActionKind('notebook');
const CodeActionTriggerKind = {Invoke: 1, Automatic: 2};

class CodeAction {
  constructor (title, kind) {
    this.title = title;
    this.kind = kind;
  }
}

class CodeLens {
  constructor (range, command) {
    this.range = range;
    this.command = command;
  }
  get isResolved () {
    return !!this.command;
  }
}

const InlayHintKind = {Type: 1, Parameter: 2};

class InlayHint {
  constructor (position, label, kind) {
    this.position = position;
    this.label = label;
    this.kind = kind;
  }
}

class InlayHintLabelPart {
  constructor (value) {
    this.value = value;
  }
}

const FoldingRangeKind = {Comment: 1, Imports: 2, Region: 3};

class FoldingRange {
  constructor (start, end, kind) {
    this.start = start;
    this.end = end;
    this.kind = kind;
  }
}

class SelectionRange {
  constructor (range, parent) {
    this.range = range;
    this.parent = parent;
  }
}

class DocumentLink {
  constructor (range, target) {
    this.range = range;
    this.target = target;
  }
}

class Color {
  constructor (red, green, blue, alpha) {
    Object.assign(this, {red, green, blue, alpha});
  }
}

class ColorInformation {
  constructor (range, color) {
    this.range = range;
    this.color = color;
  }
}

class ColorPresentation {
  constructor (label) {
    this.label = label;
  }
}

class LinkedEditingRanges {
  constructor (ranges, wordPattern) {
    this.ranges = ranges;
    this.wordPattern = wordPattern;
  }
}

class SemanticTokensLegend {
  constructor (tokenTypes, tokenModifiers) {
    this.tokenTypes = tokenTypes;
    this.tokenModifiers = tokenModifiers || [];
  }
}

class SemanticTokens {
  constructor (data, resultId) {
    this.data = data;
    this.resultId = resultId;
  }
}

class SemanticTokensBuilder {
  constructor (legend) {
    this._legend = legend;
    this._data = [];
    this._pl = 0;
    this._pc = 0;
  }
  push (a, b, c, d, e) {
    let line, ch, len, type, mods;
    if (a instanceof Range) {
      line = a.start.line;
      ch = a.start.character;
      len = a.end.character - a.start.character;
      type = this._legend ? this._legend.tokenTypes.indexOf(b) : 0;
      mods = 0;
      for (const m of c || []) mods |= 1 << (this._legend ? this._legend.tokenModifiers.indexOf(m) : 0);
    } else [line, ch, len, type, mods] = [a, b, c, d, e || 0];
    this._data.push(line - this._pl, line === this._pl ? ch - this._pc : ch, len, type, mods);
    this._pl = line;
    this._pc = ch;
  }
  build (resultId) {
    return new SemanticTokens(new Uint32Array(this._data), resultId);
  }
}

class CallHierarchyItem {
  constructor (kind, name, detail, uri, range, selectionRange) {
    Object.assign(this, {kind, name, detail, uri, range, selectionRange});
  }
}

class TypeHierarchyItem extends CallHierarchyItem {}

class ThemeIcon {
  constructor (id, color) {
    this.id = id;
    this.color = color;
  }
}
ThemeIcon.File = new ThemeIcon('file');
ThemeIcon.Folder = new ThemeIcon('folder');

class ThemeColor {
  constructor (id) {
    this.id = id;
  }
}

class RelativePattern {
  constructor (base, pattern) {
    this.baseUri = typeof base === 'string' ? Uri.file(base) : base.uri ? base.uri : base;
    this.base = this.baseUri.fsPath;
    this.pattern = pattern;
  }
}

class TreeItem {
  constructor (a, collapsibleState) {
    if (a instanceof Uri) this.resourceUri = a;
    else this.label = a;
    this.collapsibleState = collapsibleState || 0;
  }
}

class FileSystemError extends Error {
  constructor (m) {
    super(typeof m === 'string' ? m : m ? m.toString() : 'FileSystemError');
    this.code = 'Unknown';
  }
}
for (const [n, c] of [['FileNotFound', 'FileNotFound'], ['FileExists', 'FileExists'], ['FileNotADirectory', 'FileNotADirectory'],
  ['FileIsADirectory', 'FileIsADirectory'], ['NoPermissions', 'NoPermissions'], ['Unavailable', 'Unavailable']])
  FileSystemError[n] = (m) => {
    const e = new FileSystemError(m);
    e.code = c;
    return e;
  };

class TaskGroup {
  constructor (id, label) {
    this.id = id;
    this.label = label;
  }
}
TaskGroup.Build = new TaskGroup('build', 'Build');
TaskGroup.Test = new TaskGroup('test', 'Test');
TaskGroup.Clean = new TaskGroup('clean', 'Clean');
TaskGroup.Rebuild = new TaskGroup('rebuild', 'Rebuild');

class ShellExecution {
  constructor (a, b, c) {
    if (Array.isArray(b)) {
      this.command = a;
      this.args = b;
      this.options = c;
    } else {
      this.commandLine = a;
      this.options = b;
    }
  }
}

class ProcessExecution {
  constructor (process, args, options) {
    this.process = process;
    this.args = Array.isArray(args) ? args : [];
    this.options = Array.isArray(args) ? options : args;
  }
}

class Task {
  constructor (definition, scope, name, source, execution, problemMatchers) {
    Object.assign(this, {definition, scope, name, source, execution, problemMatchers});
  }
}

class DebugAdapterExecutable {
  constructor (command, args, options) {
    Object.assign(this, {command, args: args || [], options});
  }
}

class DebugAdapterServer {
  constructor (port, host) {
    this.port = port;
    this.host = host;
  }
}

class NotebookCellData {
  constructor (kind, value, languageId) {
    Object.assign(this, {kind, value, languageId});
  }
}

class NotebookData {
  constructor (cells) {
    this.cells = cells;
  }
}

class LanguageModelError extends Error {}

const enums = {
  EndOfLine, DiagnosticSeverity, DiagnosticTag, CompletionItemKind, CompletionItemTag, CompletionTriggerKind,
  SignatureHelpTriggerKind, DocumentHighlightKind, SymbolKind, SymbolTag, CodeActionTriggerKind, InlayHintKind, FoldingRangeKind,
  StatusBarAlignment: {Left: 1, Right: 2},
  ProgressLocation: {SourceControl: 1, Window: 10, Notification: 15},
  ViewColumn: {Active: -1, Beside: -2, One: 1, Two: 2, Three: 3, Four: 4, Five: 5, Six: 6, Seven: 7, Eight: 8, Nine: 9},
  TextEditorRevealType: {Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3},
  TextEditorCursorStyle: {Line: 1, Block: 2, Underline: 3, LineThin: 4, BlockOutline: 5, UnderlineThin: 6},
  TextEditorLineNumbersStyle: {Off: 0, On: 1, Relative: 2, Interval: 3},
  TextEditorSelectionChangeKind: {Keyboard: 1, Mouse: 2, Command: 3},
  TextDocumentSaveReason: {Manual: 1, AfterDelay: 2, FocusOut: 3},
  TextDocumentChangeReason: {Undo: 1, Redo: 2},
  DecorationRangeBehavior: {OpenOpen: 0, ClosedClosed: 1, OpenClosed: 2, ClosedOpen: 3},
  OverviewRulerLane: {Left: 1, Center: 2, Right: 4, Full: 7},
  ConfigurationTarget: {Global: 1, Workspace: 2, WorkspaceFolder: 3},
  FileType: {Unknown: 0, File: 1, Directory: 2, SymbolicLink: 64},
  FilePermission: {Readonly: 1},
  FileChangeType: {Changed: 1, Created: 2, Deleted: 3},
  TreeItemCollapsibleState: {None: 0, Collapsed: 1, Expanded: 2},
  ExtensionKind: {UI: 1, Workspace: 2},
  ExtensionMode: {Production: 1, Development: 2, Test: 3},
  UIKind: {Desktop: 1, Web: 2},
  ColorThemeKind: {Light: 1, Dark: 2, HighContrast: 3, HighContrastLight: 4},
  QuickPickItemKind: {Separator: -1, Default: 0},
  InputBoxValidationSeverity: {Info: 1, Warning: 2, Error: 3},
  LogLevel: {Off: 0, Trace: 1, Debug: 2, Info: 3, Warning: 4, Error: 5},
  EnvironmentVariableMutatorType: {Replace: 1, Append: 2, Prepend: 3},
  TaskRevealKind: {Always: 1, Silent: 2, Never: 3},
  TaskPanelKind: {Shared: 1, Dedicated: 2, New: 3},
  TaskScope: {Global: 1, Workspace: 2},
  ShellQuoting: {Escape: 1, Strong: 2, Weak: 3},
  DebugConsoleMode: {Separate: 0, MergeWithParent: 1},
  DebugConfigurationProviderTriggerKind: {Initial: 1, Dynamic: 2},
  NotebookCellKind: {Markup: 1, Code: 2},
  LanguageStatusSeverity: {Information: 0, Warning: 1, Error: 2},
  CommentMode: {Editing: 0, Preview: 1},
  InlineCompletionTriggerKind: {Invoke: 0, Automatic: 1},
  SemanticTokenModifiers: {}, SemanticTokenTypes: {},
  TerminalLocation: {Panel: 1, Editor: 2},
  TerminalExitReason: {Unknown: 0, Shutdown: 1, Process: 2, User: 3, Extension: 4},
  IndentAction: {None: 0, Indent: 1, IndentOutdent: 2, Outdent: 3},
  SyntaxTokenType: {Other: 0, Comment: 1, String: 2, RegEx: 3},
  PortAutoForwardAction: {Notify: 1, OpenBrowser: 2, OpenPreview: 3, Silent: 4, Ignore: 5},
};


// ----------------------------------------------------------------- settings

let settings = {};	// mme's settings.json, flat ("yaml.schemas": {...})
const defaults = {};	// the running extensions' contributes.configuration defaults
const onDidChangeConfiguration = new EventEmitter();

function settingValue (key) {
  if (Object.prototype.hasOwnProperty.call(settings, key)) return settings[key];
  if (Object.prototype.hasOwnProperty.call(defaults, key)) return defaults[key];
  // a key under an object setting ("files.associations" given as "files": {...}) or a whole section asked
  const pre = key + '.';
  let obj;
  for (const src of [defaults, settings])
    for (const k of Object.keys(src))
      if (k.startsWith(pre)) {
        obj = obj || {};
        setDeep(obj, k.slice(pre.length).split('.'), src[k]);
      }
  return obj;
}

function setDeep (o, parts, v) {
  for (let i = 0; i < parts.length - 1; i++) {
    if (typeof o[parts[i]] !== 'object' || o[parts[i]] === null) o[parts[i]] = {};
    o = o[parts[i]];
  }
  o[parts[parts.length - 1]] = v;
}

function clone (v) {
  return v === undefined ? undefined : JSON.parse(JSON.stringify(v));
}

function getConfiguration (section) {
  const pre = section ? section + '.' : '';
  const cfg = {
    get (key, def) {
      const v = settingValue(pre + key);
      return v === undefined ? def : clone(v);
    },
    has (key) {
      return settingValue(pre + key) !== undefined;
    },
    inspect (key) {
      const k = pre + key;
      return {key: k, defaultValue: clone(defaults[k]), globalValue: clone(settings[k])};
    },
    update (key, value, target) {
      const k = pre + key;
      if (value === undefined) delete settings[k];
      else settings[k] = clone(value);
      onDidChangeConfiguration.fire({affectsConfiguration: (s) => k === s || k.startsWith(s + '.') || s.startsWith(k + '.')});
      return request('mme/settingsUpdate', {key: k, value: value === undefined ? null : value, target: target || 1}).then(() => {}, () => {});
    },
  };
  // its keys as properties too, the way VS Code's WorkspaceConfiguration has them
  const whole = section ? settingValue(section) : undefined;
  if (whole && typeof whole === 'object')
    for (const k of Object.keys(whole)) if (!(k in cfg)) cfg[k] = clone(whole[k]);
  return cfg;
}


// ----------------------------------------------------------------- commands

const commands = new Map();	// id -> {fn, thisArg, ext}
const contextKeys = {};	// setContext's

function registerCommand (id, fn, thisArg) {
  if (commands.has(id)) log('[warning] command ' + id + ' registered again');
  commands.set(id, {fn, thisArg});
  return new Disposable(() => commands.delete(id));
}

async function executeCommand (id, ...args) {
  if (!commands.has(id)) await activateOn('onCommand:' + id);	// registered already: run now (an extension's own, while it activates)
  const c = commands.get(id);
  if (c) return c.fn.apply(c.thisArg, args);
  if (id === 'setContext') {
    contextKeys[args[0]] = args[1];
    return undefined;
  }
  if (id === 'vscode.open' && args[0]) return showDocument(toUri(args[0]));
  if (id === 'vscode.openFolder') return undefined;
  if (id === 'vscode.executeDocumentSymbolProvider' && args[0]) return runProviders('documentSymbol', docFor(args[0]), []);
  const r = await request('mme/executeCommand', {command: id, arguments: args.map(plain)}).catch(() => undefined);
  if (r === undefined || r === null) said('commands.executeCommand ' + id);
  return r === null ? undefined : r;
}

function plain (v) {	// what can go to mme as JSON
  try {
    return JSON.parse(JSON.stringify(v, (k, x) => (x instanceof Uri ? x.toString() : x)));
  } catch (e) {
    return null;
  }
}

// a command whose arguments are objects of ours (a code action's, a lens's): kept here, mme gets a handle
const handles = new Map();
let nextHandle = 1;
function commandToLsp (c) {
  if (!c) return undefined;
  if (typeof c === 'string') return {title: c, command: c};
  const args = c.arguments || [];
  const json = plain(args);
  if (json !== null && JSON.stringify(json) === safeJson(args)) return {title: c.title || '', command: c.command, arguments: json};
  const h = nextHandle++;
  handles.set(h, c);
  if (handles.size > 5000) handles.delete(handles.keys().next().value);
  return {title: c.title || '', command: '_mme.command', arguments: [h]};
}
registerCommand('_mme.command', (h) => {
  const c = handles.get(h);
  return c ? executeCommand(c.command, ...(c.arguments || [])) : undefined;
});


// ----------------------------------------------------------------- window

function messageType (sev) {
  return sev === 'error' ? 1 : sev === 'warning' ? 2 : 3;
}

function showMessage (sev, message, ...rest) {
  let items = rest;
  if (rest.length && rest[0] && typeof rest[0] === 'object' && !('title' in rest[0])) {
    const opts = rest[0];
    items = rest.slice(1);
    if (opts.detail) message += '\n' + opts.detail;
  }
  items = items.filter((x) => x !== undefined);
  if (items.length === 0) {
    notify('window/showMessage', {type: messageType(sev), message: String(message)});
    return Promise.resolve(undefined);
  }
  const titles = items.map((x) => (typeof x === 'string' ? x : x.title));
  return request('window/showMessageRequest', {type: messageType(sev), message: String(message), actions: titles.map((t) => ({title: t}))})
    .then((r) => (r && r.title !== undefined ? items[titles.indexOf(r.title)] : undefined), () => undefined);
}

async function showQuickPick (items, options, token) {
  items = await items;
  options = options || {};
  const list = (items || []).map((x) => (typeof x === 'string' ? {label: x} : x));
  const shown = list.filter((x) => x.kind !== -1);
  const r = await request('mme/quickPick', {
    title: options.title || '', placeHolder: options.placeHolder || '', canPickMany: !!options.canPickMany,
    items: shown.map((x) => ({label: labelText(x.label), description: x.description || '', detail: x.detail || '', picked: !!x.picked})),
  }).catch(() => null);
  if (r === null || r === undefined) return undefined;
  const pick = (i) => {
    const x = shown[i];
    return typeof items[list.indexOf(x)] === 'string' ? x.label : x;
  };
  if (options.canPickMany) return (Array.isArray(r) ? r : [r]).map(pick);
  const one = Array.isArray(r) ? r[0] : r;
  if (typeof one !== 'number' || one < 0) return undefined;
  if (options.onDidSelectItem) options.onDidSelectItem(pick(one));
  return pick(one);
}

function labelText (l) {
  return String(l || '').replace(/\$\(([\w-]+)\)\s*/g, '');	// $(icon) codicons: not drawn here
}

async function showInputBox (options, token) {
  options = options || {};
  let value = options.value || '';
  for (let i = 0; i < 5; i++) {
    const r = await request('mme/inputBox', {title: options.title || '', prompt: options.prompt || '', placeHolder: options.placeHolder || '',
      value, password: !!options.password}).catch(() => null);
    if (r === null || r === undefined) return undefined;
    if (!options.validateInput) return r;
    let bad = await options.validateInput(r);
    if (bad && typeof bad === 'object') bad = bad.message;
    if (!bad) return r;
    notify('window/showMessage', {type: 2, message: bad});
    value = r;
  }
  return undefined;
}

function createQuickPick () {
  const accept = new EventEmitter(), hide = new EventEmitter(), sel = new EventEmitter(), val = new EventEmitter(),
    act = new EventEmitter(), btn = new EventEmitter(), itemBtn = new EventEmitter();
  const qp = {
    items: [], selectedItems: [], activeItems: [], value: '', placeholder: '', title: '', busy: false, enabled: true,
    canSelectMany: false, matchOnDescription: false, matchOnDetail: false, ignoreFocusOut: false, buttons: [], step: undefined,
    totalSteps: undefined, keepScrollPosition: false, sortByLabel: true,
    onDidAccept: accept.event, onDidHide: hide.event, onDidChangeSelection: sel.event, onDidChangeValue: val.event,
    onDidChangeActive: act.event, onDidTriggerButton: btn.event, onDidTriggerItemButton: itemBtn.event,
    async show () {
      await new Promise((r) => setTimeout(r, 30));	// the items are often set just after show()
      const shown = qp.items.filter((x) => x.kind !== -1);
      const r = await request('mme/quickPick', {title: qp.title || '', placeHolder: qp.placeholder || '', canPickMany: qp.canSelectMany,
        items: shown.map((x) => ({label: labelText(x.label), description: x.description || '', detail: x.detail || '', picked: !!x.picked}))})
        .catch(() => null);
      if (r !== null && r !== undefined && !(typeof r === 'number' && r < 0)) {
        const picked = (Array.isArray(r) ? r : [r]).map((i) => shown[i]).filter(Boolean);
        qp.selectedItems = picked;
        qp.activeItems = picked;
        act.fire(picked);
        sel.fire(picked);
        accept.fire();
      }
      hide.fire();
    },
    hide () {},
    dispose () {},
  };
  return qp;
}

function createInputBox () {
  const accept = new EventEmitter(), hide = new EventEmitter(), val = new EventEmitter(), btn = new EventEmitter();
  const ib = {
    value: '', placeholder: '', prompt: '', title: '', password: false, busy: false, enabled: true, buttons: [],
    validationMessage: undefined, ignoreFocusOut: false,
    onDidAccept: accept.event, onDidHide: hide.event, onDidChangeValue: val.event, onDidTriggerButton: btn.event,
    async show () {
      const r = await request('mme/inputBox', {title: ib.title || '', prompt: ib.prompt || '', placeHolder: ib.placeholder || '',
        value: ib.value || '', password: !!ib.password}).catch(() => null);
      if (r !== null && r !== undefined) {
        ib.value = r;
        val.fire(r);
        accept.fire();
      }
      hide.fire();
    },
    hide () {},
    dispose () {},
  };
  return ib;
}

const LogLevel = enums.LogLevel;
function createOutputChannel (name, opts) {
  const isLog = !!(opts && typeof opts === 'object' && opts.log);
  const out = (text) => notify('mme/output', {channel: name, text});
  const stamp = (lvl, a) => {
    const d = new Date();
    out(d.toISOString().replace('T', ' ').replace('Z', '') + ' [' + lvl + '] ' + fmt(a) + '\n');
  };
  const ch = {
    name, logLevel: LogLevel.Info, onDidChangeLogLevel: new EventEmitter().event,
    append: (s) => out(String(s)),
    appendLine: (s) => out(String(s) + '\n'),
    replace: (s) => {
      notify('mme/output', {channel: name, text: '', clear: true});
      out(String(s));
    },
    clear: () => notify('mme/output', {channel: name, text: '', clear: true}),
    show: () => notify('mme/output', {channel: name, text: '', show: true}),
    hide: () => {},
    dispose: () => {},
  };
  if (isLog) {
    ch.trace = (...a) => stamp('trace', a);
    ch.debug = (...a) => stamp('debug', a);
    ch.info = (...a) => stamp('info', a);
    ch.warn = (...a) => stamp('warning', a);
    ch.error = (e, ...a) => stamp('error', [e, ...a]);
  }
  return ch;
}

let nextBar = 1;
function createStatusBarItem (a, b, c) {
  let id, alignment, priority;
  if (typeof a === 'string') [id, alignment, priority] = [a, b, c];
  else [alignment, priority] = [a, b];
  const key = 'sb' + nextBar++;
  const state = {text: '', tooltip: '', command: undefined, name: '', color: undefined, backgroundColor: undefined, visible: false};
  let queued = false;
  const push = () => {
    if (queued) return;
    queued = true;
    setImmediate(() => {
      queued = false;
      const tip = state.tooltip && typeof state.tooltip === 'object' ? state.tooltip.value : state.tooltip;
      notify('mme/statusBar', {id: key, name: state.name || id || '', text: labelText(state.text), tooltip: labelText(tip || ''),
        command: state.command ? '_mme.statusBar' : '', right: alignment === 2, priority: priority || 0, visible: state.visible,
        error: !!(state.backgroundColor && /error/i.test(state.backgroundColor.id || '')),
        warning: !!(state.backgroundColor && /warning/i.test(state.backgroundColor.id || ''))});
    });
  };
  bars.set(key, state);
  const item = {id: id || key, alignment: alignment || 1, priority, accessibilityInformation: undefined,
    show () {
      state.visible = true;
      push();
    },
    hide () {
      state.visible = false;
      push();
    },
    dispose () {
      state.visible = false;
      push();
      bars.delete(key);
    }};
  for (const f of ['text', 'tooltip', 'command', 'name', 'color', 'backgroundColor'])
    Object.defineProperty(item, f, {get: () => state[f], set: (v) => {
      state[f] = v;
      push();
    }, enumerable: true});
  return item;
}
const bars = new Map();
registerCommand('_mme.statusBar', (key) => {
  const s = bars.get(key);
  if (!s || !s.command) return undefined;
  const c = s.command;
  return typeof c === 'string' ? executeCommand(c) : executeCommand(c.command, ...(c.arguments || []));
});

let nextProgress = 1;
async function withProgress (options, task) {
  const token = 'hp' + nextProgress++;
  const cts = new CancellationTokenSource();
  let pct = 0;
  const title = options && options.title ? String(options.title) : '';
  await request('window/workDoneProgress/create', {token}).catch(() => {});
  notify('$/progress', {token, value: {kind: 'begin', title, cancellable: false}});
  const progress = {report (v) {
    if (v.increment) pct = Math.min(100, pct + v.increment);
    notify('$/progress', {token, value: {kind: 'report', message: v.message ? String(v.message) : undefined, percentage: pct || undefined}});
  }};
  try {
    return await task(progress, cts.token);
  } finally {
    notify('$/progress', {token, value: {kind: 'end'}});
  }
}

// the editor in front, as mme says (mme/activeEditor)
class TextEditor {
  constructor (doc, selections) {
    this.document = doc;
    this.selections = selections;
    this.options = {tabSize: 4, insertSpaces: true, cursorStyle: 1, lineNumbers: 1};
    this.viewColumn = 1;
    this.visibleRanges = [new Range(0, 0, Math.min(doc.lineCount, 60), 0)];
  }
  get selection () {
    return this.selections[0];
  }
  set selection (s) {
    this.selections = [s];
    notify('mme/select', {uri: mmeUri(this.document.uri), range: s});
  }
  edit (cb) {
    const edits = [];
    const b = {
      replace: (r, t) => edits.push(new TextEdit(r instanceof Position ? new Range(r, r) : r, t)),
      insert: (p, t) => edits.push(new TextEdit(new Range(p, p), t)),
      delete: (r) => edits.push(new TextEdit(r, '')),
      setEndOfLine: () => {},
    };
    cb(b);
    const we = new WorkspaceEdit();
    we.set(this.document.uri, edits);
    return applyEdit(we);
  }
  insertSnippet (snippet, where) {
    const pos = where instanceof Position ? new Range(where, where) : where instanceof Range ? where : this.selection;
    const we = new WorkspaceEdit();
    we.replace(this.document.uri, pos, snippet.value.replace(/\$\{\d+:([^}]*)\}/g, '$1').replace(/\$\d+/g, ''));
    return applyEdit(we);
  }
  setDecorations () {}
  revealRange (r) {
    notify('mme/select', {uri: mmeUri(this.document.uri), range: r, reveal: true});
  }
  show () {}
  hide () {}
}

let activeEditor;
const onDidChangeActiveTextEditor = new EventEmitter();
const onDidChangeVisibleTextEditors = new EventEmitter();
const onDidChangeTextEditorSelection = new EventEmitter();

function editorFor (doc) {
  if (activeEditor && activeEditor.document === doc) return activeEditor;
  return new TextEditor(doc, [new Selection(0, 0, 0, 0)]);
}

async function showDocument (uri, opts) {
  uri = toUri(uri);
  if (uri.scheme === 'http' || uri.scheme === 'https') {
    await request('window/showDocument', {uri: uri.toString(), external: true}).catch(() => {});
    return undefined;
  }
  const sel = opts && opts.selection;
  await request('window/showDocument', {uri: mmeUri(uri), takeFocus: true, selection: sel ? {start: sel.start, end: sel.end} : undefined}).catch(() => {});
  const doc = docs.get(uriKey(uri)) || await openTextDocument(uri);
  return editorFor(doc);
}

const terminals = [];
function createTerminal (a, b, c) {
  const o = typeof a === 'object' && a ? a : {name: a, shellPath: b, shellArgs: c};
  const t = {
    name: o.name || 'Terminal', processId: Promise.resolve(undefined), creationOptions: o, exitStatus: undefined, state: {isInteractedWith: false},
    shellIntegration: undefined,
    sendText (text, addNewLine) {
      notify('mme/terminalSend', {name: t.name, text: String(text) + (addNewLine === false ? '' : '\r')});
    },
    show () {
      notify('mme/terminal', {name: t.name, shellPath: o.shellPath || '', shellArgs: o.shellArgs || [], cwd: o.cwd ? toUri(o.cwd).fsPath : ''});
    },
    hide () {},
    dispose () {},
  };
  terminals.push(t);
  return t;
}


// ----------------------------------------------------------------- workspace

let folders = [];	// [{uri, name, index}]
const onDidOpenTextDocument = new EventEmitter();
const onDidCloseTextDocument = new EventEmitter();
const onDidChangeTextDocument = new EventEmitter();
const onDidSaveTextDocument = new EventEmitter();
const onWillSaveTextDocument = new EventEmitter();
const onDidChangeWorkspaceFolders = new EventEmitter();
const contentProviders = new Map();	// scheme -> provider

function docFor (u) {
  return docs.get(uriKey(toUri(u)));
}

async function openTextDocument (x) {
  if (x && typeof x === 'object' && !(x instanceof Uri) && !x.scheme) {	// {language, content}: an untitled one
    const u = new Uri('untitled', '', '/Untitled-' + nextHandle++);
    const d = new TextDocument(u, x.language || 'plaintext', 1, x.content || '');
    return d;
  }
  const uri = toUri(x);
  const open = docs.get(uriKey(uri));
  if (open) return open;
  let text;
  if (contentProviders.has(uri.scheme)) text = await contentProviders.get(uri.scheme).provideTextDocumentContent(uri, noToken);
  else text = await fs.promises.readFile(uri.fsPath, 'utf8');
  const d = new TextDocument(uri, languageOf(uri.fsPath), 1, text || '');
  return d;
}

function languageOf (file) {	// from the extensions' contributes.languages, else the extension of the file
  const base = path.basename(file).toLowerCase();
  const ext = path.extname(file).toLowerCase();
  for (const e of exts)
    for (const l of (e.pkg.contributes && e.pkg.contributes.languages) || []) {
      if ((l.filenames || []).some((n) => n.toLowerCase() === base)) return l.id;
      if ((l.extensions || []).some((x) => x.toLowerCase() === ext)) return l.id;
    }
  return {'.js': 'javascript', '.ts': 'typescript', '.json': 'json', '.yaml': 'yaml', '.yml': 'yaml', '.go': 'go', '.py': 'python',
    '.c': 'c', '.h': 'c', '.cpp': 'cpp', '.md': 'markdown', '.dart': 'dart', '.rs': 'rust', '.txt': 'plaintext'}[ext] || 'plaintext';
}

// a WorkspaceEdit as LSP's, file operations done here first
async function applyEdit (we) {
  const changes = {};
  for (const e of we._edits) {
    if (e.op) {
      try {
        if (e.op === 'create') {
          if (!(e.options.ignoreIfExists && fs.existsSync(e.target.fsPath))) {
            await fs.promises.mkdir(path.dirname(e.target.fsPath), {recursive: true});
            await fs.promises.writeFile(e.target.fsPath, '');
          }
        } else if (e.op === 'delete') await fs.promises.rm(e.target.fsPath, {recursive: !!e.options.recursive, force: !!e.options.ignoreIfNotExists});
        else if (e.op === 'rename') await fs.promises.rename(e.target.fsPath, e.to.fsPath);
      } catch (err) {
        log('[error] applyEdit: ' + err.message);
        return false;
      }
      continue;
    }
    const k = mmeUri(e.uri);
    (changes[k] = changes[k] || []).push({range: rangeToLsp(e.edit.range), newText: e.edit.newText});
  }
  if (Object.keys(changes).length === 0) return true;
  const r = await request('workspace/applyEdit', {edit: {changes}}).catch(() => null);
  return !!(r && r.applied);
}

const fsApi = {
  async stat (u) {
    const s = await fs.promises.stat(toUri(u).fsPath).catch((e) => {
      throw FileSystemError.FileNotFound(String(u));
    });
    return {type: s.isDirectory() ? 2 : 1, ctime: s.ctimeMs, mtime: s.mtimeMs, size: s.size};
  },
  async readFile (u) {
    return new Uint8Array(await fs.promises.readFile(toUri(u).fsPath).catch(() => {
      throw FileSystemError.FileNotFound(String(u));
    }));
  },
  async writeFile (u, data) {
    const f = toUri(u).fsPath;
    await fs.promises.mkdir(path.dirname(f), {recursive: true});
    await fs.promises.writeFile(f, Buffer.from(data));
  },
  async readDirectory (u) {
    const ents = await fs.promises.readdir(toUri(u).fsPath, {withFileTypes: true});
    return ents.map((d) => [d.name, d.isDirectory() ? 2 : d.isSymbolicLink() ? 64 : 1]);
  },
  async createDirectory (u) {
    await fs.promises.mkdir(toUri(u).fsPath, {recursive: true});
  },
  async delete (u, o) {
    await fs.promises.rm(toUri(u).fsPath, {recursive: !!(o && o.recursive), force: true});
  },
  async rename (a, b) {
    await fs.promises.rename(toUri(a).fsPath, toUri(b).fsPath);
  },
  async copy (a, b) {
    await fs.promises.cp(toUri(a).fsPath, toUri(b).fsPath, {recursive: true});
  },
  isWritableFileSystem: (scheme) => scheme === 'file',
};

// a glob as a RegExp over a /-separated relative path ({a,b}, **, *, ?, [..])
function globRe (g) {
  let re = '';
  let i = 0;
  let brace = 0;
  g = String(g).replace(/\\/g, '/');
  while (i < g.length) {
    const c = g[i];
    if (c === '*') {
      if (g[i + 1] === '*') {	// ** : any folders; **/ : none or some
        if (g[i + 2] === '/') {
          re += '(?:.*/)?';
          i += 3;
        } else {
          re += '.*';
          i += 2;
        }
        continue;
      }
      re += '[^/]*';
    } else if (c === '?') re += '[^/]';
    else if (c === '{') {
      re += '(?:';
      brace++;
    } else if (c === '}' && brace) {
      re += ')';
      brace--;
    } else if (c === ',' && brace) re += '|';
    else if (c === '[') {
      const j = g.indexOf(']', i);
      if (j < 0) re += '\\[';
      else {
        re += '[' + g.slice(i + 1, j).replace(/^!/, '^') + ']';
        i = j;
      }
    } else re += c.replace(/[.+^$()|\\]/g, '\\$&');
    i++;
  }
  return new RegExp('^' + re + '$', isWin ? 'i' : '');
}

function globMatch (pattern, file) {	// pattern: a string or a RelativePattern; file: a path
  let base = folders[0] ? folders[0].uri.fsPath : '';
  let g = pattern;
  if (pattern && typeof pattern === 'object') {
    base = pattern.baseUri ? pattern.baseUri.fsPath : pattern.base;
    g = pattern.pattern;
  }
  const rel = path.relative(base, file).replace(/\\/g, '/');
  if (rel.startsWith('..')) return globRe(g).test(file.replace(/\\/g, '/'));
  return globRe(g).test(rel) || (!String(g).includes('/') && globRe(g).test(path.basename(file)));
}

async function findFiles (include, exclude, maxResults, token) {
  const out = [];
  const max = maxResults || 1e9;
  const skip = (rel, name) => name === '.git' || name === 'node_modules' || (exclude && globMatch(exclude, rel));
  const roots = include && typeof include === 'object' && include.baseUri ? [include.baseUri.fsPath] : folders.map((f) => f.uri.fsPath);
  for (const root of roots) {
    const stack = [root];
    while (stack.length && out.length < max) {
      if (token && token.isCancellationRequested) return out;
      const dir = stack.pop();
      let ents;
      try {
        ents = await fs.promises.readdir(dir, {withFileTypes: true});
      } catch (e) {
        continue;
      }
      for (const d of ents) {
        const full = path.join(dir, d.name);
        if (skip(full, d.name)) continue;
        if (d.isDirectory()) stack.push(full);
        else if (globMatch(include, full)) {
          out.push(Uri.file(full));
          if (out.length >= max) break;
        }
      }
    }
  }
  return out;
}

const watchers = [];
let watching;
function createFileSystemWatcher (pattern, ignoreCreate, ignoreChange, ignoreDelete) {
  const c = new EventEmitter(), ch = new EventEmitter(), d = new EventEmitter();
  const w = {pattern, ignoreCreate, ignoreChange, ignoreDelete, c, ch, d};
  watchers.push(w);
  if (!watching && folders[0]) {
    try {
      watching = fs.watch(folders[0].uri.fsPath, {recursive: true}, (ev, name) => {
        if (!name) return;
        const full = path.join(folders[0].uri.fsPath, name);
        const exists = fs.existsSync(full);
        for (const x of watchers) {
          if (!globMatch(x.pattern, full)) continue;
          const u = Uri.file(full);
          if (!exists) {
            if (!x.ignoreDelete) x.d.fire(u);
          } else if (ev === 'rename') {
            if (!x.ignoreCreate) x.c.fire(u);
          } else if (!x.ignoreChange) x.ch.fire(u);
        }
      });
      watching.on('error', () => {});
    } catch (e) {
      said('workspace.createFileSystemWatcher (' + e.message + ')');
    }
  }
  return {ignoreCreateEvents: !!ignoreCreate, ignoreChangeEvents: !!ignoreChange, ignoreDeleteEvents: !!ignoreDelete,
    onDidCreate: c.event, onDidChange: ch.event, onDidDelete: d.event,
    dispose () {
      const i = watchers.indexOf(w);
      if (i >= 0) watchers.splice(i, 1);
    }};
}

function getWorkspaceFolder (u) {
  const f = toUri(u).fsPath.toLowerCase();
  return folders.find((w) => f.startsWith(w.uri.fsPath.toLowerCase()));
}

function asRelativePath (x, includeFolder) {
  const f = typeof x === 'string' ? x : toUri(x).fsPath;
  const w = getWorkspaceFolder(Uri.file(f));
  if (!w) return f;
  const rel = path.relative(w.uri.fsPath, f).replace(/\\/g, '/');
  return includeFolder && folders.length > 1 ? w.name + '/' + rel : rel;
}


// ----------------------------------------------------------------- languages: the providers

const providers = [];	// {kind, selector, provider, meta, ext}
let providerSeq = 0;

function register (kind, selector, provider, meta) {
  const p = {kind, selector, provider, meta: meta || {}, seq: providerSeq++};
  providers.push(p);
  languagesChanged();
  return new Disposable(() => {
    const i = providers.indexOf(p);
    if (i >= 0) providers.splice(i, 1);
    languagesChanged();
  });
}

// how well a selector fits a document (VS Code's score: 10 exact, 5 '*', 0 no)
function score (sel, doc) {
  if (!sel) return 0;
  if (Array.isArray(sel)) return Math.max(0, ...sel.map((s) => score(s, doc)));
  if (typeof sel === 'string') return sel === doc.languageId ? 10 : sel === '*' ? 5 : 0;
  let s = 0;
  if (sel.notebookType) return 0;
  if (sel.language) {
    if (sel.language === doc.languageId) s = 10;
    else if (sel.language === '*') s = 5;
    else return 0;
  }
  if (sel.scheme) {
    if (sel.scheme === doc.uri.scheme) s = Math.max(s, 10);
    else if (sel.scheme === '*') s = Math.max(s, 5);
    else return 0;
  }
  if (sel.pattern) {
    if (globMatch(sel.pattern, doc.uri.fsPath)) s = Math.max(s, 10);
    else return 0;
  }
  return s;
}

function matching (kind, doc) {
  return providers.filter((p) => p.kind === kind && score(p.selector, doc) > 0)
    .sort((a, b) => score(b.selector, doc) - score(a.selector, doc) || b.seq - a.seq);
}

// the languages the host answers for: the running extensions' onLanguage and the providers' selectors
let servedSent = '';
function languagesChanged () {
  setImmediate(() => {
    const langs = new Set();
    for (const e of exts) {
      for (const ev of e.events) if (ev.startsWith('onLanguage:')) langs.add(ev.slice(11));
    }
    for (const p of providers) {
      const sels = Array.isArray(p.selector) ? p.selector : [p.selector];
      for (const s of sels) {
        const l = typeof s === 'string' ? s : s && s.language;
        if (l && l !== '*' && p.kind !== 'diagnostics') langs.add(l);
      }
    }
    const list = [...langs].sort();
    const k = list.join(',');
    if (k !== servedSent) {
      servedSent = k;
      notify('mme/languages', {languages: list});
    }
    const ghost = new Set();
    let all = false;
    for (const p of providers) {
      if (p.kind !== 'inlineCompletion') continue;
      for (const sel of Array.isArray(p.selector) ? p.selector : [p.selector]) {
        const l = typeof sel === 'string' ? sel : sel && sel.language;
        if (l && l !== '*') ghost.add(l);
        else all = true;	// '*', a scheme or a pattern: every file
      }
    }
    const g = (all ? '*;' : '') + [...ghost].sort().join(',');
    if (g !== inlineSent) {
      inlineSent = g;
      notify('mme/inlineLanguages', {languages: [...ghost].sort(), all});
    }
  });
}
let inlineSent = '';

// diagnostics: every collection's, per file, joined
const collections = new Set();
function createDiagnosticCollection (name) {
  const map = new Map();	// uriKey -> {uri, list}
  const col = {
    name: name || 'diagnostics',
    set (a, b) {
      if (Array.isArray(a)) {
        const touched = new Set();
        for (const [u, list] of a) {
          const k = uriKey(u);
          if (!touched.has(k)) map.delete(k);
          touched.add(k);
          if (list) {
            const e = map.get(k) || {uri: toUri(u), list: []};
            e.list = e.list.concat(list);
            map.set(k, e);
          }
          publish(u);
        }
        return;
      }
      if (b) map.set(uriKey(a), {uri: toUri(a), list: [...b]});
      else map.delete(uriKey(a));
      publish(a);
    },
    delete (u) {
      map.delete(uriKey(u));
      publish(u);
    },
    clear () {
      const us = [...map.values()].map((e) => e.uri);
      map.clear();
      us.forEach(publish);
    },
    forEach (cb, thisArg) {
      for (const e of map.values()) cb.call(thisArg, e.uri, e.list, col);
    },
    get: (u) => (map.get(uriKey(u)) || {}).list,
    has: (u) => map.has(uriKey(u)),
    dispose () {
      col.clear();
      collections.delete(col);
    },
    [Symbol.iterator]: function * () {
      for (const e of map.values()) yield [e.uri, e.list];
    },
    _map: map,
  };
  collections.add(col);
  return col;
}

const publishQueued = new Map();
function publish (u) {
  const k = uriKey(u);
  publishQueued.set(k, toUri(u));
  if (publishQueued.size === 1) setImmediate(() => {
    for (const [key, uri] of publishQueued) {
      let all = [];
      for (const c of collections) if (c._map.has(key)) all = all.concat(c._map.get(key).list);
      notify('textDocument/publishDiagnostics', {uri: mmeUri(uri), diagnostics: all.map(diagToLsp)});
    }
    publishQueued.clear();
  });
}

function getDiagnostics (u) {
  if (u) {
    const k = uriKey(u);
    let all = [];
    for (const c of collections) if (c._map.has(k)) all = all.concat(c._map.get(k).list);
    return all;
  }
  const m = new Map();
  for (const c of collections) for (const e of c._map.values()) {
    const k = uriKey(e.uri);
    if (!m.has(k)) m.set(k, [e.uri, []]);
    m.get(k)[1].push(...e.list);
  }
  return [...m.values()];
}


// ----------------------------------------------------------------- vscode <-> LSP

function posToLsp (p) {
  return {line: p.line, character: p.character};
}

function rangeToLsp (r) {
  if (!r) return undefined;
  if (r instanceof Position || (r.line !== undefined && r.start === undefined)) return {start: posToLsp(r), end: posToLsp(r)};
  return {start: posToLsp(r.start), end: posToLsp(r.end)};
}

function posFromLsp (p) {
  return new Position(p.line, p.character);
}

function rangeFromLsp (r) {
  return new Range(posFromLsp(r.start), posFromLsp(r.end));
}

function markup (m) {	// a MarkdownString, a string, {language, value}, or an array of those
  if (m === undefined || m === null) return undefined;
  if (Array.isArray(m)) {
    const parts = m.map(markup).filter(Boolean).map((x) => x.value);
    return parts.length ? {kind: 'markdown', value: parts.join('\n\n---\n\n')} : undefined;
  }
  if (typeof m === 'string') return m ? {kind: 'markdown', value: m} : undefined;
  if (m.language !== undefined && m.value !== undefined && !(m instanceof MarkdownString))
    return {kind: 'markdown', value: '```' + m.language + '\n' + m.value + '\n```'};
  return m.value ? {kind: 'markdown', value: String(m.value).replace(/\$\(([\w-]+)\)/g, '')} : undefined;
}

function diagToLsp (d) {
  const code = d.code && typeof d.code === 'object' ? d.code.value : d.code;
  return {range: rangeToLsp(d.range), severity: (d.severity === undefined ? 0 : d.severity) + 1, message: String(d.message),
    source: d.source, code: code === undefined ? undefined : code, tags: d.tags,
    relatedInformation: d.relatedInformation && d.relatedInformation.map((r) => ({location: locToLsp(r.location), message: r.message}))};
}

function diagFromLsp (d) {
  const x = new Diagnostic(rangeFromLsp(d.range), d.message, (d.severity || 1) - 1);
  x.source = d.source;
  x.code = d.code;
  return x;
}

function locToLsp (l) {
  if (!l) return undefined;
  if (l.targetUri) return {uri: mmeUri(l.targetUri), range: rangeToLsp(l.targetSelectionRange || l.targetRange)};
  return {uri: mmeUri(l.uri), range: rangeToLsp(l.range)};
}

function locsToLsp (r) {
  if (!r) return [];
  return (Array.isArray(r) ? r : [r]).map(locToLsp).filter(Boolean);
}

function editsToLsp (es) {
  return (es || []).map((e) => ({range: rangeToLsp(e.range), newText: e.newText !== undefined ? e.newText : e.snippet ? e.snippet.value : ''}));
}

function workspaceEditToLsp (we) {
  if (!we) return undefined;
  const changes = {};
  for (const [u, es] of we.entries()) changes[mmeUri(u)] = editsToLsp(es);
  return {changes};
}

function completionToLsp (c, p, i) {
  const label = typeof c.label === 'string' ? c.label : c.label.label;
  const it = {label, kind: c.kind === undefined ? undefined : c.kind + 1, detail: c.detail || (typeof c.label === 'object' ? c.label.detail : undefined),
    documentation: markup(c.documentation), sortText: c.sortText, filterText: c.filterText, preselect: c.preselect,
    tags: c.tags, data: {p, i}};
  if (typeof c.label === 'object' && (c.label.detail || c.label.description))
    it.labelDetails = {detail: c.label.detail, description: c.label.description};
  const text = c.insertText === undefined ? label : c.insertText;
  if (text instanceof SnippetString) {
    it.insertText = text.value;
    it.insertTextFormat = 2;
  } else it.insertText = text;
  if (c.range) {
    const newText = it.insertText;
    if (c.range.inserting) it.textEdit = {newText, insert: rangeToLsp(c.range.inserting), replace: rangeToLsp(c.range.replacing)};
    else it.textEdit = {newText, range: rangeToLsp(c.range)};
  }
  if (c.textEdit) it.textEdit = {newText: c.textEdit.newText, range: rangeToLsp(c.textEdit.range)};
  if (c.additionalTextEdits) it.additionalTextEdits = editsToLsp(c.additionalTextEdits);
  if (c.command) it.command = commandToLsp(c.command);
  if (c.commitCharacters) it.commitCharacters = c.commitCharacters;
  return it;
}

function symbolToLsp (s) {
  if (s.selectionRange)	// a DocumentSymbol
    return {name: s.name, detail: s.detail, kind: s.kind + 1, tags: s.tags, range: rangeToLsp(s.range),
      selectionRange: rangeToLsp(s.selectionRange), children: (s.children || []).map(symbolToLsp)};
  return {name: s.name, kind: s.kind + 1, tags: s.tags, containerName: s.containerName, location: locToLsp(s.location)};
}

function actionToLsp (a) {
  if (!a) return undefined;
  if (a.command !== undefined && a.title !== undefined && typeof a.command === 'string')	// a Command
    return commandToLsp(a);
  return {title: a.title, kind: a.kind ? a.kind.value : undefined, isPreferred: a.isPreferred,
    diagnostics: a.diagnostics && a.diagnostics.map(diagToLsp), edit: workspaceEditToLsp(a.edit), command: commandToLsp(a.command),
    disabled: a.disabled};
}


// ----------------------------------------------------------------- mme asks: the providers answer

// the providers of a kind, called in turn; 'first' stops at the first answer, else all are joined
async function runProviders (kind, doc, args, first, method) {
  const out = [];
  for (const p of matching(kind, doc)) {
    const fn = p.provider[method || defaultMethod[kind]];
    if (typeof fn !== 'function') continue;
    try {
      const r = await fn.apply(p.provider, [doc, ...args]);
      if (r === undefined || r === null) continue;
      out.push({p, r});
      if (first) break;
    } catch (e) {
      if (!(e instanceof CancellationError)) log('[error] ' + kind + ' provider: ' + (e && e.stack ? e.stack : e));
    }
  }
  return out;
}

const defaultMethod = {
  completion: 'provideCompletionItems', hover: 'provideHover', signature: 'provideSignatureHelp', definition: 'provideDefinition',
  typeDefinition: 'provideTypeDefinition', implementation: 'provideImplementation', declaration: 'provideDeclaration',
  references: 'provideReferences', highlight: 'provideDocumentHighlights', documentSymbol: 'provideDocumentSymbols',
  formatting: 'provideDocumentFormattingEdits', rangeFormatting: 'provideDocumentRangeFormattingEdits',
  onTypeFormatting: 'provideOnTypeFormattingEdits', codeAction: 'provideCodeActions', codeLens: 'provideCodeLenses',
  inlayHint: 'provideInlayHints', rename: 'provideRenameEdits', folding: 'provideFoldingRanges',
  selectionRange: 'provideSelectionRanges', link: 'provideDocumentLinks', color: 'provideDocumentColors',
  linkedEditing: 'provideLinkedEditingRanges', inlineCompletion: 'provideInlineCompletionItems',
};

const completionCache = [];	// the items of the last completions, for completionItem/resolve
const lensCache = [];

function tokenFor (id) {
  const s = new CancellationTokenSource();
  if (id !== undefined) cancels.set(id, s);
  return s.token;
}

const handlers = {
  async 'textDocument/completion' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const ctx = p.context || {};
    const trig = ctx.triggerKind === 2 ? ctx.triggerCharacter : undefined;
    const vctx = {triggerKind: ctx.triggerKind === 2 ? 1 : ctx.triggerKind === 3 ? 2 : 0, triggerCharacter: trig};
    const pos = posFromLsp(p.position);
    completionCache.length = 0;
    let incomplete = false;
    const items = [];
    for (const pr of matching('completion', doc)) {
      if (trig && !(pr.meta.triggerCharacters || []).includes(trig)) continue;
      try {
        let r = await pr.provider.provideCompletionItems(doc, pos, tok, vctx);
        if (!r) continue;
        if (!Array.isArray(r)) {
          incomplete = incomplete || !!r.isIncomplete;
          r = r.items || [];
        }
        for (const c of r) {
          completionCache.push({pr, c});
          items.push(completionToLsp(c, pr.seq, completionCache.length - 1));
        }
      } catch (e) {
        if (!(e instanceof CancellationError)) log('[error] completion provider: ' + (e && e.stack ? e.stack : e));
      }
    }
    return items.length || incomplete ? {isIncomplete: incomplete, items} : null;
  },
  async 'completionItem/resolve' (item, tok) {
    const k = item.data && completionCache[item.data.i];
    if (!k || typeof k.pr.provider.resolveCompletionItem !== 'function') return item;
    try {
      const r = (await k.pr.provider.resolveCompletionItem(k.c, tok)) || k.c;
      return completionToLsp(r, item.data.p, item.data.i);
    } catch (e) {
      return item;
    }
  },
  async 'textDocument/hover' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const rs = await runProviders('hover', doc, [posFromLsp(p.position), tok]);
    const parts = [];
    let range;
    for (const {r} of rs) {
      const m = markup(r.contents);
      if (m) parts.push(m.value);
      range = range || r.range;
    }
    return parts.length ? {contents: {kind: 'markdown', value: parts.join('\n\n---\n\n')}, range: rangeToLsp(range)} : null;
  },
  async 'textDocument/signatureHelp' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const c = p.context || {};
    const [x] = await runProviders('signature', doc, [posFromLsp(p.position), tok,
      {triggerKind: c.triggerKind || 1, triggerCharacter: c.triggerCharacter, isRetrigger: !!c.isRetrigger}], true);
    if (!x) return null;
    const s = x.r;
    return {activeSignature: s.activeSignature || 0, activeParameter: s.activeParameter || 0,
      signatures: (s.signatures || []).map((g) => ({label: g.label, documentation: markup(g.documentation), activeParameter: g.activeParameter,
        parameters: (g.parameters || []).map((q) => ({label: q.label, documentation: markup(q.documentation)}))}))};
  },
  'textDocument/definition': (p, tok) => locations('definition', p, tok),
  'textDocument/typeDefinition': (p, tok) => locations('typeDefinition', p, tok),
  'textDocument/implementation': (p, tok) => locations('implementation', p, tok),
  'textDocument/declaration': (p, tok) => locations('declaration', p, tok),
  async 'textDocument/references' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const rs = await runProviders('references', doc, [posFromLsp(p.position), {includeDeclaration: !!(p.context && p.context.includeDeclaration)}, tok]);
    return rs.flatMap(({r}) => locsToLsp(r));
  },
  async 'textDocument/documentHighlight' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('highlight', doc, [posFromLsp(p.position), tok], true);
    return x ? x.r.map((h) => ({range: rangeToLsp(h.range), kind: (h.kind || 0) + 1})) : null;
  },
  async 'textDocument/documentSymbol' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('documentSymbol', doc, [tok], true);
    return x ? x.r.map(symbolToLsp) : null;
  },
  async 'workspace/symbol' (p, tok) {
    const out = [];
    for (const pr of providers.filter((q) => q.kind === 'workspaceSymbol')) {
      try {
        const r = await pr.provider.provideWorkspaceSymbols(p.query || '', tok);
        for (const s of r || []) out.push(symbolToLsp(s));
      } catch (e) {
        log('[error] workspace symbol provider: ' + e.message);
      }
    }
    return out;
  },
  async 'textDocument/formatting' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('formatting', doc, [p.options || {}, tok], true);
    return x ? editsToLsp(x.r) : null;
  },
  async 'textDocument/rangeFormatting' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('rangeFormatting', doc, [rangeFromLsp(p.range), p.options || {}, tok], true);
    return x ? editsToLsp(x.r) : null;
  },
  async 'textDocument/onTypeFormatting' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    for (const pr of matching('onTypeFormatting', doc)) {
      const chars = [pr.meta.first].concat(pr.meta.more || []);
      if (!chars.includes(p.ch)) continue;
      try {
        const r = await pr.provider.provideOnTypeFormattingEdits(doc, posFromLsp(p.position), p.ch, p.options || {}, tok);
        if (r) return editsToLsp(r);
      } catch (e) {
        log('[error] on type formatting: ' + e.message);
      }
    }
    return null;
  },
  async 'textDocument/codeAction' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const c = p.context || {};
    const vctx = {diagnostics: (c.diagnostics || []).map(diagFromLsp), only: c.only && c.only.length ? new CodeActionKind(c.only[0]) : undefined,
      triggerKind: c.triggerKind || 1};
    const rs = await runProviders('codeAction', doc, [rangeFromLsp(p.range), vctx, tok]);
    return rs.flatMap(({r}) => r.map(actionToLsp).filter(Boolean));
  },
  async 'textDocument/codeLens' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    lensCache.length = 0;
    const rs = await runProviders('codeLens', doc, [tok]);
    const out = [];
    for (const {p: pr, r} of rs)
      for (const l of r) {
        lensCache.push({pr, l});
        out.push({range: rangeToLsp(l.range), command: commandToLsp(l.command), data: {i: lensCache.length - 1}});
      }
    return out;
  },
  async 'codeLens/resolve' (lens, tok) {
    const k = lens.data && lensCache[lens.data.i];
    if (!k || typeof k.pr.provider.resolveCodeLens !== 'function') return lens;
    try {
      const r = (await k.pr.provider.resolveCodeLens(k.l, tok)) || k.l;
      return {range: rangeToLsp(r.range), command: commandToLsp(r.command), data: lens.data};
    } catch (e) {
      return lens;
    }
  },
  async 'textDocument/inlayHint' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const rs = await runProviders('inlayHint', doc, [rangeFromLsp(p.range), tok]);
    return rs.flatMap(({r}) => r.map((h) => ({position: posToLsp(h.position),
      label: typeof h.label === 'string' ? h.label : h.label.map((x) => x.value).join(''), kind: h.kind,
      paddingLeft: h.paddingLeft, paddingRight: h.paddingRight,
      tooltip: typeof h.tooltip === 'string' ? h.tooltip : h.tooltip && h.tooltip.value})));
  },
  async 'textDocument/prepareRename' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    for (const pr of matching('rename', doc)) {
      if (typeof pr.provider.prepareRename !== 'function') return rangeToLsp(doc.getWordRangeAtPosition(posFromLsp(p.position)));
      try {
        const r = await pr.provider.prepareRename(doc, posFromLsp(p.position), tok);
        if (!r) return null;
        return r.range ? {range: rangeToLsp(r.range), placeholder: r.placeholder} : rangeToLsp(r);
      } catch (e) {
        notify('window/showMessage', {type: 2, message: e.message});
        return null;
      }
    }
    return null;
  },
  async 'textDocument/rename' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('rename', doc, [posFromLsp(p.position), p.newName, tok], true);
    return x ? workspaceEditToLsp(x.r) : null;
  },
  async 'textDocument/foldingRange' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const rs = await runProviders('folding', doc, [{}, tok]);
    const kinds = {1: 'comment', 2: 'imports', 3: 'region'};
    return rs.flatMap(({r}) => r.map((f) => ({startLine: f.start, endLine: f.end, kind: kinds[f.kind]})));
  },
  async 'textDocument/selectionRange' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('selectionRange', doc, [p.positions.map(posFromLsp), tok], true);
    const conv = (s) => (s ? {range: rangeToLsp(s.range), parent: conv(s.parent)} : undefined);
    return x ? x.r.map(conv) : null;
  },
  async 'textDocument/documentLink' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const rs = await runProviders('link', doc, [tok]);
    return rs.flatMap(({r}) => r.map((l) => ({range: rangeToLsp(l.range), target: l.target ? (l.target.scheme === 'file' ? mmeUri(l.target) : l.target.toString()) : undefined,
      tooltip: l.tooltip})));
  },
  async 'textDocument/documentColor' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const rs = await runProviders('color', doc, [tok]);
    return rs.flatMap(({r}) => r.map((c) => ({range: rangeToLsp(c.range), color: {red: c.color.red, green: c.color.green, blue: c.color.blue, alpha: c.color.alpha}})));
  },
  async 'textDocument/linkedEditingRange' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const [x] = await runProviders('linkedEditing', doc, [posFromLsp(p.position), tok], true);
    return x ? {ranges: x.r.ranges.map(rangeToLsp), wordPattern: x.r.wordPattern ? x.r.wordPattern.source : undefined} : null;
  },
  async 'textDocument/inlineCompletion' (p, tok) {	// ghost text (Supermaven, Blackbox...)
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    const ctx = {triggerKind: p.context && p.context.triggerKind === 1 ? 0 : 1, selectedCompletionInfo: undefined};
    const items = [];
    for (const pr of matching('inlineCompletion', doc)) {
      try {
        let r = await pr.provider.provideInlineCompletionItems(doc, posFromLsp(p.position), ctx, tok);
        if (!r) continue;
        if (!Array.isArray(r)) r = r.items || [];
        for (const it of r) {
          const t = it.insertText !== undefined ? it.insertText : it.text;
          if (t === undefined || t === null) continue;
          items.push({insertText: t instanceof SnippetString ? {kind: 2, value: t.value} : String(t),
            range: it.range ? rangeToLsp(it.range) : undefined, command: commandToLsp(it.command)});
        }
      } catch (e) {
        if (!(e instanceof CancellationError)) log('[error] inline completion provider: ' + (e && e.stack ? e.stack : e));
      }
    }
    return {items};
  },
  async 'workspace/executeCommand' (p) {
    const r = await executeCommand(p.command, ...(p.arguments || []));
    return plain(r === undefined ? null : r);
  },
};

async function locations (kind, p, tok) {
  const doc = docFor(p.textDocument.uri);
  if (!doc) return null;
  const rs = await runProviders(kind, doc, [posFromLsp(p.position), tok]);
  const out = rs.flatMap(({r}) => locsToLsp(r));
  return out.length ? out : null;
}


// ----------------------------------------------------------------- the vscode module

const exts = [];	// {id, dir, pkg, events, active, activating, exports, ctx}
let dataDir = '';
let appRoot = '';
const onDidChangeExtensions = new EventEmitter();

function stubEvent (name) {
  return () => {
    return new Disposable(() => {});
  };
}

// a namespace whose missing members say so once and do nothing (an event returns a Disposable,
// a create/register/show returns an object that does nothing either)
function spare (ns, nsName) {
  return new Proxy(ns, {
    get (t, k) {
      if (k in t || typeof k === 'symbol') return t[k];
      const name = 'vscode.' + nsName + '.' + String(k);
      if (/^on[A-Z]/.test(k)) {
        said(name);
        return stubEvent(name);
      }
      if (/^(create|register|show|open|get|set|find|select|start|add|remove|execute|provide|update|reveal|save|apply)/.test(k)) {
        said(name);
        return (...a) => nothing(name);
      }
      if (/^[A-Z]/.test(k)) {	// a class or an enum not made: new X() gives a harmless object
        said(name);
        t[k] = class {};
        return t[k];
      }
      return undefined;	// a property: VS Code's may be undefined too (env.remoteName)
    },
  });
}

function nothing (name) {	// what a missing create* gives: every call and property is harmless
  const f = function () {
    return nothing(name);
  };
  return new Proxy(f, {
    get (t, k) {
      if (k === 'then') return undefined;	// not a promise
      if (k === 'dispose') return () => {};
      if (k === Symbol.toPrimitive) return () => '';
      if (typeof k === 'string' && /^on[A-Z]/.test(k)) return stubEvent(name + '.' + k);
      return nothing(name + '.' + String(k));
    },
    apply () {
      return nothing(name);
    },
    construct () {
      return nothing(name);
    },
  });
}

function reg (kind) {
  return (selector, provider, ...meta) => {
    let m = {};
    if (kind === 'completion') m.triggerCharacters = meta.flat().filter((x) => typeof x === 'string');
    else if (kind === 'onTypeFormatting') m = {first: meta[0], more: meta.slice(1)};
    else if (kind === 'signature') m = meta[0] && typeof meta[0] === 'object' ? meta[0] : {triggerCharacters: meta};
    else if (meta[0] && typeof meta[0] === 'object') m = meta[0];
    return register(kind, selector, provider, m);
  };
}

const window = spare({
  showInformationMessage: (...a) => showMessage('info', ...a),
  showWarningMessage: (...a) => showMessage('warning', ...a),
  showErrorMessage: (...a) => showMessage('error', ...a),
  showQuickPick, showInputBox, createQuickPick, createInputBox, createOutputChannel, createStatusBarItem, withProgress,
  showTextDocument: (x, o) => showDocument(x instanceof TextDocument ? x.uri : x, o && typeof o === 'object' ? o : undefined),
  get activeTextEditor () {
    return activeEditor;
  },
  get visibleTextEditors () {
    return activeEditor ? [activeEditor] : [];
  },
  onDidChangeActiveTextEditor: onDidChangeActiveTextEditor.event,
  onDidChangeVisibleTextEditors: onDidChangeVisibleTextEditors.event,
  onDidChangeTextEditorSelection: onDidChangeTextEditorSelection.event,
  onDidChangeTextEditorVisibleRanges: stubEvent(),
  onDidChangeTextEditorOptions: stubEvent(),
  onDidChangeTextEditorViewColumn: stubEvent(),
  onDidChangeWindowState: stubEvent(),
  onDidChangeActiveColorTheme: stubEvent(),
  onDidOpenTerminal: stubEvent(),
  onDidCloseTerminal: stubEvent(),
  onDidChangeActiveTerminal: stubEvent(),
  onDidChangeTerminalState: stubEvent(),
  onDidChangeTerminalShellIntegration: stubEvent(),
  onDidStartTerminalShellExecution: stubEvent(),
  onDidEndTerminalShellExecution: stubEvent(),
  state: {focused: true, active: true},
  activeColorTheme: {kind: 2},
  get terminals () {
    return terminals;
  },
  get activeTerminal () {
    return terminals[terminals.length - 1];
  },
  createTerminal,
  setStatusBarMessage (text, a) {
    const it = createStatusBarItem(1, -1000);
    it.text = text;
    it.show();
    const d = new Disposable(() => it.dispose());
    if (typeof a === 'number') setTimeout(() => d.dispose(), a);
    else if (a && typeof a.then === 'function') a.then(() => d.dispose(), () => d.dispose());
    return d;
  },
  createTextEditorDecorationType: () => ({key: 'dec' + nextHandle++, dispose () {}}),
  registerUriHandler: () => new Disposable(() => {}),
  tabGroups: {all: [], activeTabGroup: {tabs: [], activeTab: undefined, isActive: true, viewColumn: 1},
    onDidChangeTabGroups: stubEvent(), onDidChangeTabs: stubEvent(), close: async () => true},
  async showOpenDialog (o) {
    const r = await showInputBox({title: (o && o.title) || 'Open', prompt: 'A path', value: folders[0] ? folders[0].uri.fsPath : ''});
    return r ? [Uri.file(r)] : undefined;
  },
  async showSaveDialog (o) {
    const r = await showInputBox({title: (o && o.title) || 'Save As', prompt: 'A path', value: o && o.defaultUri ? o.defaultUri.fsPath : ''});
    return r ? Uri.file(r) : undefined;
  },
  async showWorkspaceFolderPick () {
    if (folders.length <= 1) return folders[0];
    const r = await showQuickPick(folders.map((f) => f.name));
    return folders.find((f) => f.name === r);
  },
}, 'window');

const workspace = spare({
  get workspaceFolders () {
    return folders.length ? folders : undefined;
  },
  get name () {
    return folders[0] ? folders[0].name : undefined;
  },
  get rootPath () {
    return folders[0] ? folders[0].uri.fsPath : undefined;
  },
  workspaceFile: undefined,
  get textDocuments () {
    return [...docs.values()];
  },
  notebookDocuments: [],
  getConfiguration,
  onDidChangeConfiguration: onDidChangeConfiguration.event,
  onDidOpenTextDocument: onDidOpenTextDocument.event,
  onDidCloseTextDocument: onDidCloseTextDocument.event,
  onDidChangeTextDocument: onDidChangeTextDocument.event,
  onDidSaveTextDocument: onDidSaveTextDocument.event,
  onWillSaveTextDocument: onWillSaveTextDocument.event,
  onDidChangeWorkspaceFolders: onDidChangeWorkspaceFolders.event,
  onDidCreateFiles: stubEvent(), onDidDeleteFiles: stubEvent(), onDidRenameFiles: stubEvent(),
  onWillCreateFiles: stubEvent(), onWillDeleteFiles: stubEvent(), onWillRenameFiles: stubEvent(),
  onDidGrantWorkspaceTrust: stubEvent(),
  onDidOpenNotebookDocument: stubEvent(), onDidCloseNotebookDocument: stubEvent(), onDidChangeNotebookDocument: stubEvent(),
  onDidSaveNotebookDocument: stubEvent(),
  isTrusted: true,
  openTextDocument, applyEdit, findFiles, createFileSystemWatcher, getWorkspaceFolder, asRelativePath,
  fs: fsApi,
  registerTextDocumentContentProvider (scheme, p) {
    contentProviders.set(scheme, p);
    return new Disposable(() => contentProviders.delete(scheme));
  },
  saveAll: () => executeCommand('workbench.action.files.saveAll').then(() => true, () => false),
  save: (u) => (docFor(u) ? docFor(u).save().then(() => toUri(u)) : Promise.resolve(undefined)),
  updateWorkspaceFolders: () => false,
  registerTaskProvider: (type, p) => tasks.registerTaskProvider(type, p),
}, 'workspace');

const languages = spare({
  createDiagnosticCollection,
  getDiagnostics,
  onDidChangeDiagnostics: stubEvent(),
  match: (sel, doc) => score(sel, doc),
  async getLanguages () {
    const s = new Set(['plaintext']);
    for (const e of exts) for (const l of (e.pkg.contributes && e.pkg.contributes.languages) || []) s.add(l.id);
    for (const d of docs.values()) s.add(d.languageId);
    return [...s];
  },
  setTextDocumentLanguage: async (doc, id) => {
    doc.languageId = id;
    return doc;
  },
  setLanguageConfiguration: () => new Disposable(() => {}),
  createLanguageStatusItem (id, selector) {
    const it = createStatusBarItem(id, 2, 100);
    it.selector = selector;
    it.severity = 0;
    it.busy = false;
    it.detail = '';
    return it;
  },
  registerCompletionItemProvider: reg('completion'),
  registerHoverProvider: reg('hover'),
  registerSignatureHelpProvider: reg('signature'),
  registerDefinitionProvider: reg('definition'),
  registerTypeDefinitionProvider: reg('typeDefinition'),
  registerImplementationProvider: reg('implementation'),
  registerDeclarationProvider: reg('declaration'),
  registerReferenceProvider: reg('references'),
  registerDocumentHighlightProvider: reg('highlight'),
  registerDocumentSymbolProvider: reg('documentSymbol'),
  registerWorkspaceSymbolProvider: (p) => register('workspaceSymbol', '*', p),
  registerDocumentFormattingEditProvider: reg('formatting'),
  registerDocumentRangeFormattingEditProvider: reg('rangeFormatting'),
  registerOnTypeFormattingEditProvider: reg('onTypeFormatting'),
  registerCodeActionsProvider: reg('codeAction'),
  registerCodeLensProvider: reg('codeLens'),
  registerInlayHintsProvider: reg('inlayHint'),
  registerRenameProvider: reg('rename'),
  registerFoldingRangeProvider: reg('folding'),
  registerSelectionRangeProvider: reg('selectionRange'),
  registerDocumentLinkProvider: reg('link'),
  registerColorProvider: reg('color'),
  registerLinkedEditingRangeProvider: reg('linkedEditing'),
  registerInlineCompletionItemProvider: reg('inlineCompletion'),
}, 'languages');

const commandsNs = spare({
  registerCommand,
  registerTextEditorCommand: (id, fn, thisArg) => registerCommand(id, (...a) => {
    if (!activeEditor) return undefined;
    return fn.call(thisArg, activeEditor, {replace () {}, insert () {}, delete () {}, setEndOfLine () {}}, ...a);
  }),
  executeCommand,
  getCommands: async (filterInternal) => [...commands.keys()].filter((k) => !filterInternal || !k.startsWith('_')),
}, 'commands');

const env = spare({
  appName: 'mme', get appRoot () {
    return appRoot;
  },
  appHost: 'desktop', uriScheme: 'mme', language: 'en', machineId: crypto.createHash('sha256').update(os.hostname()).digest('hex'),
  sessionId: crypto.randomUUID(), isNewAppInstall: false, isTelemetryEnabled: false, uiKind: 1, remoteName: undefined,
  get shell () {
    return isWin ? (process.env.ComSpec || 'cmd.exe') : (process.env.SHELL || '/bin/sh');
  },
  logLevel: 3, onDidChangeLogLevel: stubEvent(), onDidChangeTelemetryEnabled: stubEvent(), onDidChangeShell: stubEvent(),
  telemetryConfiguration: {isUsageEnabled: false, isErrorsEnabled: false, isCrashEnabled: false},
  createTelemetryLogger: () => ({logUsage () {}, logError () {}, dispose () {}, onDidChangeEnableStates: stubEvent(), isUsageEnabled: false, isErrorsEnabled: false}),
  openExternal: (u) => request('window/showDocument', {uri: toUri(u).toString(), external: true}).then(() => true, () => false),
  asExternalUri: async (u) => u,
  clipboard: {
    readText: () => request('mme/clipboard', {}).then((s) => s || '', () => ''),
    writeText: (s) => request('mme/clipboard', {text: String(s)}).then(() => {}, () => {}),
  },
}, 'env');

const extensionsNs = spare({
  getExtension: (id) => {
    const e = exts.find((x) => x.id === String(id).toLowerCase());
    return e ? extensionObject(e) : undefined;
  },
  get all () {
    return exts.map(extensionObject);
  },
  onDidChange: onDidChangeExtensions.event,
}, 'extensions');

const tasks = spare({
  registerTaskProvider: (type, p) => {
    said('vscode.tasks.registerTaskProvider (' + type + ')');
    return new Disposable(() => {});
  },
  fetchTasks: async () => [],
  taskExecutions: [],
  onDidStartTask: stubEvent(), onDidEndTask: stubEvent(), onDidStartTaskProcess: stubEvent(), onDidEndTaskProcess: stubEvent(),
}, 'tasks');

const debug = spare({
  activeDebugSession: undefined, activeDebugConsole: {append () {}, appendLine () {}}, breakpoints: [],
  onDidStartDebugSession: stubEvent(), onDidTerminateDebugSession: stubEvent(), onDidChangeActiveDebugSession: stubEvent(),
  onDidReceiveDebugSessionCustomEvent: stubEvent(), onDidChangeBreakpoints: stubEvent(), onDidChangeActiveStackItem: stubEvent(),
}, 'debug');

const l10n = {
  t (m, ...a) {
    const msg = typeof m === 'object' ? m.message : m;
    const args = typeof m === 'object' ? m.args || {} : a.length === 1 && typeof a[0] === 'object' && a[0] !== null ? a[0] : a;
    return String(msg).replace(/\{(\w+)\}/g, (all, k) => (args[k] !== undefined ? String(args[k]) : all));
  },
  bundle: undefined, uri: undefined,
};

const vscodeApi = spare({
  version: '1.108.0',
  Uri, Position, Range, Selection, Location, Disposable, EventEmitter, CancellationTokenSource, CancellationError,
  TextEdit, SnippetTextEdit, WorkspaceEdit, SnippetString, MarkdownString, Diagnostic, DiagnosticRelatedInformation, Hover,
  VerboseHover, CompletionItem, CompletionList, InlineCompletionItem, InlineCompletionList, SignatureHelp, SignatureInformation,
  ParameterInformation, DocumentHighlight, SymbolInformation, DocumentSymbol, CodeActionKind, CodeAction, CodeLens, InlayHint,
  InlayHintLabelPart, FoldingRange, SelectionRange, DocumentLink, Color, ColorInformation, ColorPresentation, LinkedEditingRanges,
  SemanticTokensLegend, SemanticTokens, SemanticTokensBuilder, CallHierarchyItem, TypeHierarchyItem, ThemeIcon, ThemeColor,
  RelativePattern, TreeItem, FileSystemError, TaskGroup, ShellExecution, ProcessExecution, Task, DebugAdapterExecutable,
  DebugAdapterServer, NotebookCellData, NotebookData, LanguageModelError, TextDocument, TextLine,
  ...enums,
  window, workspace, languages, commands: commandsNs, env, extensions: extensionsNs, tasks, debug, l10n,
  scm: spare({createSourceControl: undefined, inputBox: undefined}, 'scm'),
  comments: spare({}, 'comments'), authentication: spare({onDidChangeSessions: stubEvent()}, 'authentication'),
  notebooks: spare({}, 'notebooks'), tests: spare({}, 'tests'), chat: spare({}, 'chat'), lm: spare({tools: []}, 'lm'),
}, '');


// ----------------------------------------------------------------- extensions: loading, activating

function extensionObject (e) {
  return {id: e.pkg.publisher + '.' + e.pkg.name, extensionUri: Uri.file(e.dir), extensionPath: e.dir, packageJSON: e.pkg,
    extensionKind: 2, get isActive () {
      return e.active;
    }, get exports () {
      return e.exports;
    }, activate: () => activate(e).then(() => e.exports)};
}

function memento (file) {
  let data = {};
  try {
    data = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    data = {};
  }
  return {
    get: (k, d) => (data[k] === undefined ? d : clone(data[k])),
    keys: () => Object.keys(data),
    async update (k, v) {
      if (v === undefined) delete data[k];
      else data[k] = clone(v);
      await fs.promises.mkdir(path.dirname(file), {recursive: true});
      await fs.promises.writeFile(file, JSON.stringify(data));
    },
    setKeysForSync () {},
  };
}

function makeContext (e) {
  const state = path.join(dataDir, 'extension-state', e.id);
  const ws = folders[0] ? crypto.createHash('md5').update(folders[0].uri.fsPath.toLowerCase()).digest('hex').slice(0, 12) : 'none';
  const secretsFile = path.join(state, 'secrets.json');
  const secrets = memento(secretsFile);
  const secretsEv = new EventEmitter();
  return {
    subscriptions: [],
    extensionPath: e.dir, extensionUri: Uri.file(e.dir), extension: extensionObject(e), extensionMode: 1,
    asAbsolutePath: (p) => path.join(e.dir, p),
    globalState: memento(path.join(state, 'global.json')),
    workspaceState: memento(path.join(state, 'ws-' + ws + '.json')),
    secrets: {get: async (k) => secrets.get(k), store: (k, v) => secrets.update(k, v), delete: (k) => secrets.update(k, undefined),
      keys: async () => secrets.keys(), onDidChange: secretsEv.event},
    storagePath: path.join(state, 'ws-' + ws), storageUri: Uri.file(path.join(state, 'ws-' + ws)),
    globalStoragePath: path.join(state, 'global'), globalStorageUri: Uri.file(path.join(state, 'global')),
    logPath: path.join(state, 'logs'), logUri: Uri.file(path.join(state, 'logs')),
    environmentVariableCollection: {persistent: true, description: '', replace () {}, append () {}, prepend () {}, get () {},
      forEach () {}, delete () {}, clear () {}, getScoped () {
        return this;
      }},
    languageModelAccessInformation: {onDidChange: stubEvent(), canSendRequest: () => undefined},
  };
}

// require('vscode') anywhere under an extension: the API above
const realLoad = Module._load;
Module._load = function (req, parent, isMain) {
  if (req === 'vscode') return vscodeApi;
  return realLoad.apply(this, arguments);
};

async function activate (e) {
  if (e.active || e.failed) return;
  if (e.activating) return e.activating;
  let done;
  e.activating = new Promise((r) => {	// set before its code runs: what it runs may ask for it again
    done = r;
  });
  (async () => {
    for (const dep of e.pkg.extensionDependencies || []) {
      const d = exts.find((x) => x.id === dep.toLowerCase());
      if (d) await activate(d);
    }
    const main = e.pkg.main;
    const t0 = Date.now();
    try {
      e.ctx = makeContext(e);
      if (main) {
        const mod = require(path.resolve(e.dir, main));
        e.module = mod;
        if (typeof mod.activate === 'function') e.exports = await mod.activate(e.ctx);
      }
      e.active = true;
      log('[info] activated ' + e.id + ' in ' + (Date.now() - t0) + ' ms');
      notify('mme/extensionState', {id: e.id, state: 'running'});
    } catch (err) {
      e.failed = true;
      log('[error] ' + e.id + ' failed to activate: ' + (err && err.stack ? err.stack : err));
      notify('mme/extensionState', {id: e.id, state: 'error', message: String(err && err.message ? err.message : err)});
    }
  })().finally(done);
  return e.activating;
}

// an activation event happened: every extension waiting for it starts
async function activateOn (ev) {
  const waiting = exts.filter((e) => !e.active && !e.failed && e.events.some((x) => x === ev || x === '*'));
  for (const e of waiting) await activate(e);
}

function readExtension (dir) {
  const pkg = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
  const id = (pkg.publisher + '.' + pkg.name).toLowerCase();
  const events = [...(pkg.activationEvents || [])];
  const c = pkg.contributes || {};
  for (const cmd of c.commands || []) events.push('onCommand:' + cmd.command);	// implicit, VS Code 1.74+
  for (const l of c.languages || []) if (l.id) events.push('onLanguage:' + l.id);
  const props = [];
  const confs = Array.isArray(c.configuration) ? c.configuration : c.configuration ? [c.configuration] : [];
  for (const cf of confs) for (const [k, v] of Object.entries(cf.properties || {})) {
    if (v && v.default !== undefined && defaults[k] === undefined) defaults[k] = v.default;
    props.push(k);
  }
  for (const [k, v] of Object.entries(c.configurationDefaults || {})) if (!k.startsWith('[')) defaults[k] = v;
  return {id, dir, pkg, events, active: false, activating: null, exports: undefined, props};
}


// ----------------------------------------------------------------- mme tells: documents, settings, the editor

async function didOpen (p) {
  const t = p.textDocument;
  const uri = Uri.parse(t.uri);
  mmeUris.set(uriKey(uri), t.uri);
  const d = new TextDocument(uri, t.languageId, t.version || 1, t.text || '');
  docs.set(uriKey(uri), d);
  await activateOn('onLanguage:' + t.languageId);
  onDidOpenTextDocument.fire(d);
}

function didChange (p) {
  const d = docFor(p.textDocument.uri);
  if (!d) return;
  const before = d._text;
  const changes = [];
  for (const c of p.contentChanges || []) {
    if (c.range) {
      const r = rangeFromLsp(c.range);
      const a = d.offsetAt(r.start), b = d.offsetAt(r.end);
      d._set(d._text.slice(0, a) + c.text + d._text.slice(b));
      changes.push({range: r, rangeOffset: a, rangeLength: b - a, text: c.text});
    } else {
      const end = d.positionAt(before.length);
      d._set(c.text);
      changes.push({range: new Range(new Position(0, 0), end), rangeOffset: 0, rangeLength: before.length, text: c.text});
    }
  }
  d.version = p.textDocument.version || d.version + 1;
  d.isDirty = true;
  onDidChangeTextDocument.fire({document: d, contentChanges: changes, reason: undefined});
}

function didSave (p) {
  const d = docFor(p.textDocument.uri);
  if (!d) return;
  if (p.text !== undefined) d._set(p.text);
  d.isDirty = false;
  onDidSaveTextDocument.fire(d);
}

function didClose (p) {
  const d = docFor(p.textDocument.uri);
  if (!d) return;
  docs.delete(uriKey(d.uri));
  d.isClosed = true;
  onDidCloseTextDocument.fire(d);
  if (activeEditor && activeEditor.document === d) {
    activeEditor = undefined;
    onDidChangeActiveTextEditor.fire(undefined);
  }
}

function activeEditorChanged (p) {	// mme/activeEditor {uri, selection} | {}
  const d = p && p.uri ? docFor(p.uri) : undefined;
  const sel = p && p.selection ? new Selection(posFromLsp(p.selection.start), posFromLsp(p.selection.end)) : new Selection(0, 0, 0, 0);
  if (!d) {
    if (activeEditor) {
      activeEditor = undefined;
      onDidChangeActiveTextEditor.fire(undefined);
      onDidChangeVisibleTextEditors.fire([]);
    }
    return;
  }
  if (activeEditor && activeEditor.document === d) {
    if (!activeEditor.selection.isEqual(sel)) {
      activeEditor.selections = [sel];
      onDidChangeTextEditorSelection.fire({textEditor: activeEditor, selections: [sel], kind: 1});
    }
    return;
  }
  activeEditor = new TextEditor(d, [sel]);
  onDidChangeActiveTextEditor.fire(activeEditor);
  onDidChangeVisibleTextEditors.fire([activeEditor]);
}

function settingsChanged (all) {
  const old = settings;
  settings = all || {};
  const changed = new Set();
  for (const k of new Set([...Object.keys(old), ...Object.keys(settings)]))
    if (safeJson(old[k]) !== safeJson(settings[k])) changed.add(k);
  if (changed.size === 0) return;
  onDidChangeConfiguration.fire({affectsConfiguration: (s) => [...changed].some((k) => k === s || k.startsWith(s + '.') || s.startsWith(k + '.'))});
}


// ----------------------------------------------------------------- starting

async function start () {
  let init;
  try {
    init = await request('mme/hostInit', {});
  } catch (e) {
    log('[error] mme did not say what to run: ' + e.message);
    return;
  }
  dataDir = init.dataDir || os.tmpdir();
  appRoot = init.appRoot || dataDir;
  settings = init.settings || {};
  folders = (init.folders || []).map((f, i) => ({uri: Uri.file(f), name: path.basename(f), index: i}));
  for (const dir of init.extensions || []) {
    try {
      exts.push(readExtension(dir));
    } catch (e) {
      log('[error] ' + dir + ': ' + e.message);
    }
  }
  log('[info] node ' + process.version + ', ' + exts.length + ' extension(s): ' + exts.map((e) => e.id).join(', '));
  // the palette's commands: what the running extensions contribute
  const cmds = [];
  for (const e of exts) for (const c of (e.pkg.contributes && e.pkg.contributes.commands) || []) {
    const t = typeof c.title === 'object' ? c.title.value : c.title;
    const cat = typeof c.category === 'object' ? c.category.value : c.category;
    cmds.push({id: c.command, title: l10nString(e, t), category: l10nString(e, cat) || '', ext: e.id});
  }
  const keys = [];
  for (const e of exts) for (const k of (e.pkg.contributes && e.pkg.contributes.keybindings) || []) {
    const key = isWin ? k.win || k.key : process.platform === 'darwin' ? k.mac || k.key : k.linux || k.key;
    if (key && k.command) keys.push({key, command: k.command, when: k.when || ''});
  }
  const props = [];
  for (const e of exts) {
    const c = e.pkg.contributes || {};
    const confs = Array.isArray(c.configuration) ? c.configuration : c.configuration ? [c.configuration] : [];
    for (const cf of confs) for (const [k, v] of Object.entries(cf.properties || {}))
      props.push({key: k, type: Array.isArray(v.type) ? v.type[0] : v.type || 'string', default: v.default === undefined ? null : v.default,
        enum: v.enum, description: l10nString(e, v.markdownDescription || v.description || '') || '', ext: e.id});
  }
  notify('mme/contributions', {commands: cmds, keybindings: keys, configuration: props});
  languagesChanged();
  for (const d of docs.values()) await activateOn('onLanguage:' + d.languageId);
  await activateOn('*');
  for (const e of exts) for (const ev of e.events)
    if (ev.startsWith('workspaceContains:') && folders[0]) {
      const found = await findFiles(ev.slice(18), undefined, 1);
      if (found.length) await activate(e);
    }
  setTimeout(() => activateOn('onStartupFinished'), 500);
}

// "%key%" titles: the extension's package.nls.json has the words
function l10nString (e, s) {
  if (typeof s !== 'string') return s;
  const m = /^%(.+)%$/.exec(s);
  if (!m) return s;
  if (!e.nls) {
    try {
      e.nls = JSON.parse(fs.readFileSync(path.join(e.dir, 'package.nls.json'), 'utf8'));
    } catch (err) {
      e.nls = {};
    }
  }
  const v = e.nls[m[1]];
  return typeof v === 'object' && v ? v.message : v || s;
}


// ----------------------------------------------------------------- the messages from mme

async function dispatch (msg) {
  if (msg.id !== undefined && msg.method === undefined) {	// an answer to one of ours
    const w = pending.get(msg.id);
    if (w) {
      pending.delete(msg.id);
      if (msg.error) w.rej(new Error(msg.error.message || 'error'));
      else w.res(msg.result);
    }
    return;
  }
  const {method, params, id} = msg;
  if (id !== undefined) {	// a request
    if (method === 'initialize') {
      send({jsonrpc: '2.0', id, result: {capabilities: capabilities(), serverInfo: {name: 'mme-exthost', version: '1'}}});
      return;
    }
    if (method === 'shutdown') {
      for (const e of exts) {
        try {
          if (e.active && e.module && typeof e.module.deactivate === 'function') await e.module.deactivate();
          for (const s of (e.ctx && e.ctx.subscriptions) || []) if (s && typeof s.dispose === 'function') s.dispose();
        } catch (err) {
          log('[error] ' + e.id + ' deactivate: ' + err.message);
        }
      }
      send({jsonrpc: '2.0', id, result: null});
      return;
    }
    const h = handlers[method];
    if (!h) {
      send({jsonrpc: '2.0', id, error: {code: -32601, message: 'not handled: ' + method}});
      return;
    }
    const tok = tokenFor(id);
    try {
      const r = await h(params || {}, tok);
      send({jsonrpc: '2.0', id, result: r === undefined ? null : r});
    } catch (e) {
      log('[error] ' + method + ': ' + (e && e.stack ? e.stack : e));
      send({jsonrpc: '2.0', id, error: {code: -32603, message: String(e && e.message ? e.message : e)}});
    } finally {
      cancels.delete(id);
    }
    return;
  }
  switch (method) {	// a notification
    case 'initialized': start(); break;
    case 'exit': process.exit(0); break;
    case '$/cancelRequest': {
      const s = cancels.get(params.id);
      if (s) s.cancel();
      break;
    }
    case 'textDocument/didOpen': didOpen(params); break;
    case 'textDocument/didChange': didChange(params); break;
    case 'textDocument/didSave': didSave(params); break;
    case 'textDocument/didClose': didClose(params); break;
    case 'workspace/didChangeConfiguration': settingsChanged(params && params.settings); break;
    case 'mme/activeEditor': activeEditorChanged(params); break;
    default: break;
  }
}

function capabilities () {
  const trig = ['.', ':', '<', '"', '\'', '/', '@', '(', ',', '-', '$', '#', '=', '>', '[', '{', '*', '\\', '&', '!'];
  return {
    positionEncoding: 'utf-16',
    textDocumentSync: {openClose: true, change: 2, save: {includeText: false}},
    completionProvider: {triggerCharacters: trig, resolveProvider: true},
    hoverProvider: true, signatureHelpProvider: {triggerCharacters: ['(', ','], retriggerCharacters: [')']},
    definitionProvider: true, typeDefinitionProvider: true, implementationProvider: true, declarationProvider: true,
    referencesProvider: true, documentHighlightProvider: true, documentSymbolProvider: true, workspaceSymbolProvider: true,
    documentFormattingProvider: true, documentRangeFormattingProvider: true,
    documentOnTypeFormattingProvider: {firstTriggerCharacter: '}', moreTriggerCharacter: [';', '\n']},
    codeActionProvider: true, codeLensProvider: {resolveProvider: true}, inlayHintProvider: true,
    renameProvider: {prepareProvider: true}, foldingRangeProvider: true, selectionRangeProvider: true,
    documentLinkProvider: {resolveProvider: false}, colorProvider: true, linkedEditingRangeProvider: true,
    inlineCompletionProvider: true,
    executeCommandProvider: {commands: []},
  };
}
