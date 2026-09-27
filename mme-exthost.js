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

// mme's end of the pipe gone (it was killed): nothing is written any more - an error written about the
// write that failed would fail too, and round again for ever, a core busy - and the host goes
let outDead = false;
process.stdout.on('error', () => {
  outDead = true;
  quit();
});

function send (msg) {
  if (outDead || process.stdout.destroyed) return;
  const s = JSON.stringify(msg);
  try {
    stdout('Content-Length: ' + Buffer.byteLength(s, 'utf8') + '\r\n\r\n' + s);
  } catch (e) {
    outDead = true;
    quit();
  }
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
process.stdin.on('end', () => quit());

// the processes the extensions start (language servers, daemons, agents): kept, so none outlives the host
const spawned = new Set();
for (const fn of ['spawn', 'execFile', 'exec', 'fork']) {
  const real = cp[fn];
  const wrap = function (...a) {
    const c = real.apply(this, a);
    if (c && c.pid) {
      spawned.add(c);
      c.once('exit', () => spawned.delete(c));
    }
    return c;
  };
  if (fn === 'exec' || fn === 'execFile') {	// util.promisify(cp.execFile) gives {stdout, stderr}, as Node's own does
    wrap[require('util').promisify.custom] = (...a) => {
      let child;
      const p = new Promise((resolve, reject) => {
        child = wrap(...a, (err, stdout, stderr) => {
          if (err) {
            err.stdout = stdout;
            err.stderr = stderr;
            reject(err);
          } else resolve({stdout, stderr});
        });
      });
      p.child = child;
      return p;
    };
  }
  cp[fn] = wrap;
}

// mme gone without a word (killed: its pipe may stay open, a child holding it): the host goes too
setInterval(() => {
  try {
    process.kill(process.ppid, 0);
  } catch (e) {
    quit();
  }
}, 2000).unref();

// the host goes (mme said exit, or its pipe closed): the extensions' deactivate, then what they started
let quitting = false;
async function quit () {
  if (quitting) return;
  quitting = true;
  const done = new Promise((r) => setTimeout(r, 1500));	// an extension that does not finish is not waited for
  const deact = Promise.all(exts.filter((e) => e.active && e.module && typeof e.module.deactivate === 'function').map((e) => {
    try {
      return Promise.resolve(e.module.deactivate()).catch(() => {});
    } catch (err) {
      return Promise.resolve();
    }
  }));
  await Promise.race([deact, done]);
  for (const c of spawned) {
    try {
      if (isWin) cp.spawnSync('taskkill', ['/PID', String(c.pid), '/T', '/F'], {stdio: 'ignore', windowsHide: true});
      else c.kill('SIGKILL');
    } catch (err) {
      // gone already
    }
  }
  process.exit(0);
}


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
process.on('uncaughtException', (e) => {
  if (e && /^(EPIPE|EOF|ECONNRESET|ERR_STREAM_DESTROYED|ERR_STREAM_WRITE_AFTER_END)$/.test(e.code || '') && (outDead || process.stdout.destroyed || e.syscall === 'write')) {
    outDead = true;	// mme is gone: not logged (the log is that pipe), the host goes
    quit();
    return;
  }
  log('[error] ' + (e && e.stack ? e.stack : e));
});
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
    if (this.authority || this.scheme === 'file') s += '//' + enc(this.authority, false).replace(/%3A(\d+)$/, ':$1');	// host:port
    let p = this.path;
    const drive = /^\/([a-zA-Z]):(.*)$/.exec(p);
    if (drive) s += '/' + drive[1].toLowerCase() + (skipEncoding ? ':' : '%3A') + enc(drive[2], true);
    else s += enc(p, true);
    if (this.query) s += '?' + (skipEncoding ? this.query : pctEncode(this.query, true).replace(/%(3D|26|2B|2C|3B|3A|40)/g, (m, h) => String.fromCharCode(parseInt(h, 16))));	// = & + , ; : @ stay, as VS Code's
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
    if (typeof scope === 'string') {	// the old form: (definition, name, source, execution, problemMatchers)
      problemMatchers = execution;
      execution = source;
      source = name;
      name = scope;
      scope = 2;
    }
    Object.assign(this, {definition, scope, name, source, execution, problemMatchers: problemMatchers || []});
    this.group = undefined;
    this.detail = undefined;
    this.isBackground = false;
    this.presentationOptions = {};
    this.runOptions = {};
  }
}

class CustomExecution {
  constructor (callback) {
    this.callback = callback;
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

class LanguageModelError extends Error {
  constructor (message, code) {
    super(message || 'language model error');
    this.code = code || 'Unknown';
  }
  static NoPermissions (m) {
    return new LanguageModelError(m || 'no permission to use the language model', 'NoPermissions');
  }
  static Blocked (m) {
    return new LanguageModelError(m || 'the request was blocked', 'Blocked');
  }
  static NotFound (m) {
    return new LanguageModelError(m || 'no such language model', 'NotFound');
  }
}

// more of VS Code's classes, as vscode.d.ts has them: an extension extends or makes them (Dart's FileCoverage),
// and a bundler's __importStar copies the module's own properties once, so each must be really there
class SemanticTokensEdit {
  constructor (start, deleteCount, data) {
    Object.assign(this, {start, deleteCount, data});
  }
}
class SemanticTokensEdits {
  constructor (edits, resultId) {
    Object.assign(this, {edits, resultId});
  }
}
class CallHierarchyIncomingCall {
  constructor (from, fromRanges) {
    Object.assign(this, {from, fromRanges});
  }
}
class CallHierarchyOutgoingCall {
  constructor (to, fromRanges) {
    Object.assign(this, {to, fromRanges});
  }
}
class FileDecoration {
  constructor (badge, tooltip, color) {
    Object.assign(this, {badge, tooltip, color, propagate: false});
  }
}
class Breakpoint {
  constructor (enabled, condition, hitCondition, logMessage) {
    Object.assign(this, {id: crypto.randomUUID(), enabled: enabled !== false, condition, hitCondition, logMessage});
  }
}
class SourceBreakpoint extends Breakpoint {
  constructor (location, enabled, condition, hitCondition, logMessage) {
    super(enabled, condition, hitCondition, logMessage);
    this.location = location;
  }
}
class FunctionBreakpoint extends Breakpoint {
  constructor (functionName, enabled, condition, hitCondition, logMessage) {
    super(enabled, condition, hitCondition, logMessage);
    this.functionName = functionName;
  }
}
class EvaluatableExpression {
  constructor (range, expression) {
    Object.assign(this, {range, expression});
  }
}
class InlineValueText {
  constructor (range, text) {
    Object.assign(this, {range, text});
  }
}
class InlineValueVariableLookup {
  constructor (range, variableName, caseSensitiveLookup) {
    Object.assign(this, {range, variableName, caseSensitiveLookup: caseSensitiveLookup !== false});
  }
}
class InlineValueEvaluatableExpression {
  constructor (range, expression) {
    Object.assign(this, {range, expression});
  }
}
class NotebookEdit {
  constructor (range, newCells) {
    Object.assign(this, {range, newCells});
  }
  static replaceCells (range, newCells) {
    return new NotebookEdit(range, newCells);
  }
  static insertCells (index, newCells) {
    return new NotebookEdit({start: index, end: index}, newCells);
  }
  static deleteCells (range) {
    return new NotebookEdit(range, []);
  }
  static updateCellMetadata (index, newCellMetadata) {
    const e = new NotebookEdit({start: index, end: index}, []);
    e.newCellMetadata = newCellMetadata;
    return e;
  }
  static updateNotebookMetadata (newNotebookMetadata) {
    const e = new NotebookEdit({start: 0, end: 0}, []);
    e.newNotebookMetadata = newNotebookMetadata;
    return e;
  }
}
class NotebookCellStatusBarItem {
  constructor (text, alignment) {
    Object.assign(this, {text, alignment});
  }
}
class TestCoverageCount {
  constructor (covered, total) {
    Object.assign(this, {covered, total});
  }
}
class FileCoverage {
  constructor (uri, statementCoverage, branchCoverage, declarationCoverage, includesTests) {
    Object.assign(this, {uri, statementCoverage, branchCoverage, declarationCoverage, includesTests: includesTests || []});
  }
  static fromDetails (uri, details) {
    const st = new TestCoverageCount(0, 0);
    for (const d of details || []) {
      if (d instanceof DeclarationCoverage) continue;
      st.total++;
      if (d.executed) st.covered++;
    }
    return new FileCoverage(uri, st);
  }
}
class StatementCoverage {
  constructor (executed, location, branches) {
    Object.assign(this, {executed, location, branches: branches || []});
  }
}
class BranchCoverage {
  constructor (executed, location, label) {
    Object.assign(this, {executed, location, label});
  }
}
class DeclarationCoverage {
  constructor (name, executed, location) {
    Object.assign(this, {name, executed, location});
  }
}
class TestMessageStackFrame {
  constructor (label, uri, position) {
    Object.assign(this, {label, uri, position});
  }
}
class TerminalLink {
  constructor (startIndex, length, tooltip) {
    Object.assign(this, {startIndex, length, tooltip});
  }
}
class TerminalProfile {
  constructor (options) {
    this.options = options;
  }
}
class DocumentDropOrPasteEditKind {
  constructor (value) {
    this.value = value;
  }
  append (...parts) {
    return new DocumentDropOrPasteEditKind((this.value ? [this.value] : []).concat(parts).join('.'));
  }
  intersects (other) {
    return this.contains(other) || other.contains(this);
  }
  contains (other) {
    return this.value === other.value || other.value.startsWith(this.value + '.');
  }
}
DocumentDropOrPasteEditKind.Empty = new DocumentDropOrPasteEditKind('');
DocumentDropOrPasteEditKind.Text = new DocumentDropOrPasteEditKind('text');
DocumentDropOrPasteEditKind.TextUpdateImports = new DocumentDropOrPasteEditKind('text.updateImports');
class DocumentDropEdit {
  constructor (insertText, title, kind) {
    Object.assign(this, {insertText, title, kind});
  }
}
class DocumentPasteEdit {
  constructor (insertText, title, kind) {
    Object.assign(this, {insertText, title, kind});
  }
}
class DataTransferItem {
  constructor (value) {
    this.value = value;
  }
  async asString () {
    return typeof this.value === 'string' ? this.value : JSON.stringify(this.value);
  }
  asFile () {
    return undefined;
  }
}
class DataTransfer {
  constructor () {
    this._m = new Map();
  }
  get (mime) {
    return this._m.get(String(mime).toLowerCase());
  }
  set (mime, item) {
    this._m.set(String(mime).toLowerCase(), item);
  }
  forEach (cb, thisArg) {
    for (const [k, v] of this._m) cb.call(thisArg, v, k, this);
  }
  [Symbol.iterator] () {
    return this._m.entries();
  }
}
class TabInputText {
  constructor (uri) {
    this.uri = uri;
  }
}
class TabInputTextDiff {
  constructor (original, modified) {
    Object.assign(this, {original, modified});
  }
}
class TabInputCustom {
  constructor (uri, viewType) {
    Object.assign(this, {uri, viewType});
  }
}
class TabInputWebview {
  constructor (viewType) {
    this.viewType = viewType;
  }
}
class TabInputNotebook {
  constructor (uri, notebookType) {
    Object.assign(this, {uri, notebookType});
  }
}
class TabInputNotebookDiff {
  constructor (original, modified, notebookType) {
    Object.assign(this, {original, modified, notebookType});
  }
}
class TabInputTerminal {}
class ChatResponseMarkdownPart {
  constructor (value) {
    this.value = typeof value === 'string' ? new MarkdownString(value) : value;
  }
}
class ChatResponseFileTreePart {
  constructor (value, baseUri) {
    Object.assign(this, {value, baseUri});
  }
}
class ChatResponseAnchorPart {
  constructor (value, title) {
    Object.assign(this, {value, title});
  }
}
class ChatResponseProgressPart {
  constructor (value) {
    this.value = value;
  }
}
class ChatResponseReferencePart {
  constructor (value, iconPath) {
    Object.assign(this, {value, iconPath});
  }
}
class ChatResponseCommandButtonPart {
  constructor (value) {
    this.value = value;
  }
}
class ChatRequestTurn {
  constructor (prompt, command, references, participant, toolReferences) {
    Object.assign(this, {prompt, command, references: references || [], participant, toolReferences: toolReferences || []});
  }
}
class ChatResponseTurn {
  constructor (response, result, participant, command) {
    Object.assign(this, {response, result, participant, command});
  }
}
class ChatReferenceBinaryData {
  constructor (mimeType, data) {
    Object.assign(this, {mimeType, data});
  }
}
class McpStdioServerDefinition {
  constructor (label, command, args, env, version) {
    Object.assign(this, {label, command, args: args || [], env: env || {}, version});
  }
}
class McpHttpServerDefinition {
  constructor (label, uri, headers, version) {
    Object.assign(this, {label, uri, headers: headers || {}, version});
  }
}
class DebugThread {
  constructor (session, threadId) {
    Object.assign(this, {session, threadId});
  }
}
class DebugStackFrame {
  constructor (session, threadId, frameId) {
    Object.assign(this, {session, threadId, frameId});
  }
}
const moreClasses = {
  SemanticTokensEdit, SemanticTokensEdits, CallHierarchyIncomingCall, CallHierarchyOutgoingCall, FileDecoration, Breakpoint,
  SourceBreakpoint, FunctionBreakpoint, EvaluatableExpression, InlineValueText, InlineValueVariableLookup,
  InlineValueEvaluatableExpression, NotebookEdit, NotebookCellStatusBarItem, TestCoverageCount, FileCoverage, StatementCoverage,
  BranchCoverage, DeclarationCoverage, TestMessageStackFrame, TerminalLink, TerminalProfile, DocumentDropOrPasteEditKind,
  DocumentDropEdit, DocumentPasteEdit, DataTransferItem, DataTransfer, TabInputText, TabInputTextDiff, TabInputCustom,
  TabInputWebview, TabInputNotebook, TabInputNotebookDiff, TabInputTerminal, ChatResponseMarkdownPart, ChatResponseFileTreePart,
  ChatResponseAnchorPart, ChatResponseProgressPart, ChatResponseReferencePart, ChatResponseCommandButtonPart, ChatRequestTurn,
  ChatResponseTurn, ChatReferenceBinaryData, McpStdioServerDefinition, McpHttpServerDefinition, DebugThread, DebugStackFrame,
  HoverVerbosityAction: {Increase: 0, Decrease: 1},
  TreeItemCheckboxState: {Unchecked: 0, Checked: 1},
  TerminalShellExecutionCommandLineConfidence: {Low: 0, Medium: 1, High: 2},
  QuickInputButtons: {Back: {iconPath: new ThemeIcon('arrow-left'), tooltip: 'Back'}},
  CommentThreadCollapsibleState: {Collapsed: 0, Expanded: 1},
  CommentThreadState: {Unresolved: 0, Resolved: 1},
  DocumentPasteTriggerKind: {Automatic: 0, PasteAs: 1},
  ChatResultFeedbackKind: {Unhelpful: 0, Helpful: 1},
};

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
  TestRunProfileKind: {Run: 1, Debug: 2, Coverage: 3},
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
const defaults = {	// the running extensions' contributes.configuration defaults, and VS Code's own they read (Codeium: http.proxy)
  'http.proxy': '', 'http.proxyAuthorization': null, 'http.proxyStrictSSL': true, 'http.proxySupport': 'override',
  'http.systemCertificates': true, 'http.noProxy': [],
};
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
  if (!section)	// getConfiguration().http: every section, asked when read
    for (const top of new Set([...Object.keys(defaults), ...Object.keys(settings)].map((k) => k.split('.')[0])))
      if (!(top in cfg)) Object.defineProperty(cfg, top, {get: () => clone(settingValue(top)), enumerable: true});
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
  const vc = await viewCommand(id);	// "<view>.focus": a webview view, in the browser
  if (vc !== undefined) return vc;
  const ce = await customEditorCommand(id);	// a custom editor for the file in front, in the browser
  if (ce !== undefined) return ce;
  if (id.startsWith('_mme.chat.')) return askParticipant(id.slice(10));	// "Ask @name..."

  if (id === 'vscode.openWith' && args[0] && args[1]) return openCustomEditor(String(args[1]), toUri(args[0]));
  if (id === 'setContext') {
    contextKeys[args[0]] = args[1];
    return undefined;
  }
  if (id === 'vscode.open' && args[0]) return showDocument(toUri(args[0]));
  if (id === 'testing.showMostRecentOutput') return notify('mme/testShowOutput', {});	// mme's Test Results
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
  return iconText(l);	// $(icon) codicons: their glyphs, mme's font has them
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
const progressCancels = new Map();	// token -> its CancellationTokenSource, while it runs
async function withProgress (options, task) {
  const token = 'hp' + nextProgress++;
  const cts = new CancellationTokenSource();
  let pct = 0;
  const title = options && options.title ? String(options.title) : '';
  const cancellable = !!(options && options.cancellable);
  await request('window/workDoneProgress/create', {token}).catch(() => {});
  if (cancellable) progressCancels.set(token, cts);
  notify('$/progress', {token, value: {kind: 'begin', title: cancellable && !/cancel/i.test(title) ? title + ' (click to cancel)' : title, cancellable}});
  const progress = {report (v) {
    if (v.increment) pct = Math.min(100, pct + v.increment);
    notify('$/progress', {token, value: {kind: 'report', message: v.message ? String(v.message) : undefined, percentage: pct || undefined}});
  }};
  try {
    return await task(progress, cts.token);
  } finally {
    progressCancels.delete(token);
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
    return (await fs.promises.readFile(toUri(u).fsPath).catch(() => {	// a Buffer, as VS Code's: its toString() is the text
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

// semantic tokens: the host's one legend (VS Code's standard types and modifiers); a provider's are put in it,
// a type it does not have left out (the positions after it made again, relative to the token before)
const SEM_TYPES = ['namespace', 'class', 'enum', 'interface', 'struct', 'typeParameter', 'type', 'parameter', 'variable',
  'property', 'enumMember', 'decorator', 'event', 'function', 'method', 'macro', 'label', 'comment', 'string', 'keyword',
  'number', 'regexp', 'operator'];
const SEM_MODS = ['declaration', 'definition', 'readonly', 'static', 'deprecated', 'abstract', 'async', 'modification',
  'documentation', 'defaultLibrary'];

function semRemap (data, legend) {
  const types = ((legend && legend.tokenTypes) || []).map((t) => SEM_TYPES.indexOf(t));
  const mods = ((legend && legend.tokenModifiers) || []).map((m) => SEM_MODS.indexOf(m));
  const out = [];
  let line = 0, ch = 0, outLine = 0, outCh = 0;
  for (let i = 0; i + 4 < data.length; i += 5) {
    const dl = data[i];
    line += dl;
    ch = dl ? data[i + 1] : ch + data[i + 1];
    const t = types[data[i + 3]];
    if (t === undefined || t < 0) continue;
    let m = 0;
    for (let b = 0, bits = data[i + 4]; bits; b++, bits >>>= 1)
      if ((bits & 1) && mods[b] >= 0) m |= 1 << mods[b];
    out.push(line - outLine, line === outLine ? ch - outCh : ch, data[i + 2], t, m);
    outLine = line;
    outCh = ch;
  }
  return out;
}

// call and type hierarchy items: kept here, mme gets a handle in data
const hierCache = new Map();
let hierSeq = 1;
function hierToLsp (it, pr) {
  const h = hierSeq++;
  hierCache.set(h, {it, pr});
  if (hierCache.size > 5000) hierCache.delete(hierCache.keys().next().value);
  return {name: it.name, kind: (it.kind || 0) + 1, detail: it.detail, tags: it.tags, uri: mmeUri(it.uri),
    range: rangeToLsp(it.range), selectionRange: rangeToLsp(it.selectionRange || it.range), data: {h}};
}

async function hierPrepare (kind, method, p, tok) {
  const doc = docFor(p.textDocument.uri);
  if (!doc) return null;
  for (const pr of matching(kind, doc)) {
    try {
      const r = await pr.provider[method](doc, posFromLsp(p.position), tok);
      const list = [].concat(r || []);
      if (list.length) return list.map((it) => hierToLsp(it, pr));
    } catch (e) {
      // the next one
    }
  }
  return null;
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
        if (l && l !== '*' && p.kind !== 'inlineCompletion') langs.add(l);	// ghost text alone does not take a language from its server
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
  async 'textDocument/semanticTokens/full' (p, tok) {	// an extension's (vscode-languageclient's gopls ...), in the host's legend
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    for (const pr of matching('semanticTokens', doc)) {
      try {
        const r = await pr.provider.provideDocumentSemanticTokens(doc, tok);
        if (r && r.data) return {data: semRemap(r.data, pr.meta)};
      } catch (e) {
        // the next one, if any
      }
    }
    return null;
  },
  async 'textDocument/semanticTokens/range' (p, tok) {
    const doc = docFor(p.textDocument.uri);
    if (!doc) return null;
    for (const pr of matching('semanticTokensRange', doc)) {
      try {
        const r = await pr.provider.provideDocumentRangeSemanticTokens(doc, rangeFromLsp(p.range), tok);
        if (r && r.data) return {data: semRemap(r.data, pr.meta)};
      } catch (e) {
        // the whole document's then
      }
    }
    return handlers['textDocument/semanticTokens/full']({textDocument: p.textDocument}, tok);
  },
  async 'textDocument/prepareCallHierarchy' (p, tok) {
    return hierPrepare('callHierarchy', 'prepareCallHierarchy', p, tok);
  },
  async 'callHierarchy/incomingCalls' (p, tok) {
    const k = hierCache.get(p.item && p.item.data && p.item.data.h);
    if (!k) return null;
    const r = (await k.pr.provider.provideCallHierarchyIncomingCalls(k.it, tok)) || [];
    return r.map((c) => ({from: hierToLsp(c.from, k.pr), fromRanges: (c.fromRanges || []).map(rangeToLsp)}));
  },
  async 'callHierarchy/outgoingCalls' (p, tok) {
    const k = hierCache.get(p.item && p.item.data && p.item.data.h);
    if (!k) return null;
    const r = (await k.pr.provider.provideCallHierarchyOutgoingCalls(k.it, tok)) || [];
    return r.map((c) => ({to: hierToLsp(c.to, k.pr), fromRanges: (c.fromRanges || []).map(rangeToLsp)}));
  },
  async 'textDocument/prepareTypeHierarchy' (p, tok) {
    return hierPrepare('typeHierarchy', 'prepareTypeHierarchy', p, tok);
  },
  async 'typeHierarchy/supertypes' (p, tok) {
    const k = hierCache.get(p.item && p.item.data && p.item.data.h);
    if (!k) return null;
    return ((await k.pr.provider.provideTypeHierarchySupertypes(k.it, tok)) || []).map((x) => hierToLsp(x, k.pr));
  },
  async 'typeHierarchy/subtypes' (p, tok) {
    const k = hierCache.get(p.item && p.item.data && p.item.data.h);
    if (!k) return null;
    return ((await k.pr.provider.provideTypeHierarchySubtypes(k.it, tok)) || []).map((x) => hierToLsp(x, k.pr));
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

// vscode.authentication: the providers extensions register (Codeium's sign in), asked for their sessions
const authProviders = new Map();	// id -> {label, provider}
const onDidChangeSessions = new EventEmitter();
const authentication = {
  registerAuthenticationProvider (id, label, provider) {
    authProviders.set(id, {label, provider});
    const sub = provider.onDidChangeSessions ? provider.onDidChangeSessions(() => onDidChangeSessions.fire({provider: {id, label}})) : null;
    return new Disposable(() => {
      authProviders.delete(id);
      if (sub) sub.dispose();
    });
  },
  async getSession (id, scopes, options) {
    const p = authProviders.get(id);
    const o = options || {};
    if (!p) {	// VS Code's own (github, microsoft): none here
      said('authentication.getSession ' + id);
      return undefined;
    }
    const list = o.forceNewSession ? [] : await p.provider.getSessions(scopes || [], {});
    if (list && list.length) return list[0];
    if (o.createIfNone || o.forceNewSession) return p.provider.createSession(scopes || [], {});
    return undefined;
  },
  async getAccounts (id) {
    const p = authProviders.get(id);
    return p ? (await p.provider.getSessions([], {}) || []).map((x) => x.account) : [];
  },
  onDidChangeSessions: onDidChangeSessions.event,
};

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

// ----------------------------------------------------------------- codicons: $(name) as its glyph

// @vscode/codicons' names and code points (mme's font has them at the same places)
const CODICONS = [
  'add:ea60,plus:ea60,gist-new:ea60,repo-create:ea60,lightbulb:ea61,light-bulb:ea61,repo:ea62,repo-delete:ea62,gist-fork:ea63,repo-forked:ea63,git-pull-request:ea64,git-pull-request-abandoned:ea64,record-keys:ea65,keyboard:ea65,tag:ea66,tag-add:ea66,tag-remove:ea66,person:ea67,person-follow:ea67,person-outline:ea67,person-filled:ea67,git-branch:ea68,git-branch-create:ea68,git-branch-delete:ea68,source-control:ea68,mirror:ea69,mirror-public:ea69,star:ea6a,star-add:ea6a,star-delete:ea6a,star-empty:ea6a,comment:ea6b,comment-add:ea6b,alert:ea6c,warning:ea6c,search:ea6d,search-save:ea6d,log-out:ea6e,sign-out:ea6e,log-in:ea6f',
  'sign-in:ea6f,eye:ea70,eye-unwatch:ea70,eye-watch:ea70,circle-filled:ea71,primitive-dot:ea71,close-dirty:ea71,debug-breakpoint:ea71,debug-breakpoint-disabled:ea71,debug-hint:ea71,primitive-square:ea72,edit:ea73,pencil:ea73,info:ea74,issue-opened:ea74,gist-private:ea75,git-fork-private:ea75,lock:ea75,mirror-private:ea75,close:ea76,remove-close:ea76,x:ea76,repo-sync:ea77,sync:ea77,clone:ea78,desktop-download:ea78,beaker:ea79,microscope:ea79,vm:ea7a,device-desktop:ea7a,file:ea7b,file-text:ea7b,more:ea7c,ellipsis:ea7c,kebab-horizontal:ea7c,mail-reply:ea7d,reply:ea7d,organization:ea7e,organization-filled:ea7e,organization-outline:ea7e',
  'new-file:ea7f,file-add:ea7f,new-folder:ea80,file-directory-create:ea80,trash:ea81,trashcan:ea81,history:ea82,clock:ea82,folder:ea83,file-directory:ea83,symbol-folder:ea83,logo-github:ea84,mark-github:ea84,github:ea84,terminal:ea85,console:ea85,repl:ea85,zap:ea86,symbol-event:ea86,error:ea87,stop:ea87,variable:ea88,symbol-variable:ea88,array:ea8a,symbol-array:ea8a,symbol-module:ea8b,symbol-package:ea8b,symbol-namespace:ea8b,symbol-object:ea8b,symbol-method:ea8c,symbol-function:ea8c,symbol-constructor:ea8c,symbol-boolean:ea8f,symbol-null:ea8f,symbol-numeric:ea90,symbol-number:ea90,symbol-structure:ea91,symbol-struct:ea91,symbol-parameter:ea92,symbol-type-parameter:ea92',
  'symbol-key:ea93,symbol-text:ea93,symbol-reference:ea94,go-to-file:ea94,symbol-enum:ea95,symbol-value:ea95,symbol-ruler:ea96,symbol-unit:ea96,activate-breakpoints:ea97,archive:ea98,arrow-both:ea99,arrow-down:ea9a,arrow-left:ea9b,arrow-right:ea9c,arrow-small-down:ea9d,arrow-small-left:ea9e,arrow-small-right:ea9f,arrow-small-up:eaa0,arrow-up:eaa1,bell:eaa2,bold:eaa3,book:eaa4,bookmark:eaa5,debug-breakpoint-conditional-unverified:eaa6,debug-breakpoint-conditional:eaa7,debug-breakpoint-conditional-disabled:eaa7,debug-breakpoint-data-unverified:eaa8,debug-breakpoint-data:eaa9,debug-breakpoint-data-disabled:eaa9,debug-breakpoint-log-unverified:eaaa,debug-breakpoint-log:eaab,debug-breakpoint-log-disabled:eaab,briefcase:eaac,broadcast:eaad,browser:eaae,bug:eaaf,calendar:eab0,case-sensitive:eab1,check:eab2,checklist:eab3',
  'chevron-down:eab4,chevron-left:eab5,chevron-right:eab6,chevron-up:eab7,chrome-close:eab8,chrome-maximize:eab9,chrome-minimize:eaba,chrome-restore:eabb,circle-outline:eabc,debug-breakpoint-unverified:eabc,circle-slash:eabd,circuit-board:eabe,clear-all:eabf,clippy:eac0,close-all:eac1,cloud-download:eac2,cloud-upload:eac3,code:eac4,collapse-all:eac5,color-mode:eac6,comment-discussion:eac7,credit-card:eac9,dash:eacc,dashboard:eacd,database:eace,debug-continue:eacf,debug-disconnect:ead0,debug-pause:ead1,debug-restart:ead2,debug-start:ead3,debug-step-into:ead4,debug-step-out:ead5,debug-step-over:ead6,debug-stop:ead7,debug:ead8,device-camera-video:ead9,device-camera:eada,device-mobile:eadb,diff-added:eadc,diff-ignored:eadd',
  'diff-modified:eade,diff-removed:eadf,diff-renamed:eae0,diff:eae1,discard:eae2,editor-layout:eae3,empty-window:eae4,exclude:eae5,extensions:eae6,eye-closed:eae7,file-binary:eae8,file-code:eae9,file-media:eaea,file-pdf:eaeb,file-submodule:eaec,file-symlink-directory:eaed,file-symlink-file:eaee,file-zip:eaef,files:eaf0,filter:eaf1,flame:eaf2,fold-down:eaf3,fold-up:eaf4,fold:eaf5,folder-active:eaf6,folder-opened:eaf7,gear:eaf8,gift:eaf9,gist-secret:eafa,gist:eafb,git-commit:eafc,git-compare:eafd,compare-changes:eafd,git-merge:eafe,github-action:eaff,github-alt:eb00,globe:eb01,grabber:eb02,graph:eb03,gripper:eb04',
  'heart:eb05,home:eb06,horizontal-rule:eb07,hubot:eb08,inbox:eb09,issue-reopened:eb0b,issues:eb0c,italic:eb0d,jersey:eb0e,json:eb0f,kebab-vertical:eb10,key:eb11,law:eb12,lightbulb-autofix:eb13,link-external:eb14,link:eb15,list-ordered:eb16,list-unordered:eb17,live-share:eb18,loading:eb19,location:eb1a,mail-read:eb1b,mail:eb1c,markdown:eb1d,megaphone:eb1e,mention:eb1f,milestone:eb20,mortar-board:eb21,move:eb22,multiple-windows:eb23,mute:eb24,no-newline:eb25,note:eb26,octoface:eb27,open-preview:eb28,package:eb29,paintcan:eb2a,pin:eb2b,play:eb2c,run:eb2c',
  'plug:eb2d,preserve-case:eb2e,preview:eb2f,project:eb30,pulse:eb31,question:eb32,quote:eb33,radio-tower:eb34,reactions:eb35,references:eb36,refresh:eb37,regex:eb38,remote-explorer:eb39,remote:eb3a,remove:eb3b,replace-all:eb3c,replace:eb3d,repo-clone:eb3e,repo-force-push:eb3f,repo-pull:eb40,repo-push:eb41,report:eb42,request-changes:eb43,rocket:eb44,root-folder-opened:eb45,root-folder:eb46,rss:eb47,ruby:eb48,save-all:eb49,save-as:eb4a,save:eb4b,screen-full:eb4c,screen-normal:eb4d,search-stop:eb4e,server:eb50,settings-gear:eb51,settings:eb52,shield:eb53,smiley:eb54,sort-precedence:eb55',
  'split-horizontal:eb56,split-vertical:eb57,squirrel:eb58,star-full:eb59,star-half:eb5a,symbol-class:eb5b,symbol-color:eb5c,symbol-constant:eb5d,symbol-enum-member:eb5e,symbol-field:eb5f,symbol-file:eb60,symbol-interface:eb61,symbol-keyword:eb62,symbol-misc:eb63,symbol-operator:eb64,symbol-property:eb65,wrench:eb65,wrench-subaction:eb65,symbol-snippet:eb66,tasklist:eb67,telescope:eb68,text-size:eb69,three-bars:eb6a,thumbsdown:eb6b,thumbsup:eb6c,tools:eb6d,triangle-down:eb6e,triangle-left:eb6f,triangle-right:eb70,triangle-up:eb71,twitter:eb72,unfold:eb73,unlock:eb74,unmute:eb75,unverified:eb76,verified:eb77,versions:eb78,vm-active:eb79,vm-outline:eb7a,vm-running:eb7b',
  'watch:eb7c,whitespace:eb7d,whole-word:eb7e,window:eb7f,word-wrap:eb80,zoom-in:eb81,zoom-out:eb82,list-filter:eb83,list-flat:eb84,list-selection:eb85,selection:eb85,list-tree:eb86,debug-breakpoint-function-unverified:eb87,debug-breakpoint-function:eb88,debug-breakpoint-function-disabled:eb88,debug-stackframe-active:eb89,circle-small-filled:eb8a,debug-stackframe-dot:eb8a,debug-stackframe:eb8b,debug-stackframe-focused:eb8b,debug-breakpoint-unsupported:eb8c,symbol-string:eb8d,debug-reverse-continue:eb8e,debug-step-back:eb8f,debug-restart-frame:eb90,debug-alt:eb91,call-incoming:eb92,call-outgoing:eb93,menu:eb94,expand-all:eb95,feedback:eb96,group-by-ref-type:eb97,ungroup-by-ref-type:eb98,account:eb99,bell-dot:eb9a,debug-console:eb9b,library:eb9c,output:eb9d,run-all:eb9e,sync-ignored:eb9f',
  'pinned:eba0,github-inverted:eba1,server-process:eba2,server-environment:eba3,pass:eba4,issue-closed:eba4,stop-circle:eba5,play-circle:eba6,record:eba7,debug-alt-small:eba8,vm-connect:eba9,cloud:ebaa,merge:ebab,export:ebac,graph-left:ebad,magnet:ebae,notebook:ebaf,redo:ebb0,check-all:ebb1,pinned-dirty:ebb2,pass-filled:ebb3,circle-large-filled:ebb4,circle-large-outline:ebb5,combine:ebb6,gather:ebb6,table:ebb7,variable-group:ebb8,type-hierarchy:ebb9,type-hierarchy-sub:ebba,type-hierarchy-super:ebbb,git-pull-request-create:ebbc,run-above:ebbd,run-below:ebbe,notebook-template:ebbf,debug-rerun:ebc0,workspace-trusted:ebc1,workspace-untrusted:ebc2,workspace-unknown:ebc3,terminal-cmd:ebc4,terminal-debian:ebc5',
  'terminal-linux:ebc6,terminal-powershell:ebc7,terminal-tmux:ebc8,terminal-ubuntu:ebc9,terminal-bash:ebca,arrow-swap:ebcb,copy:ebcc,person-add:ebcd,filter-filled:ebce,wand:ebcf,debug-line-by-line:ebd0,inspect:ebd1,layers:ebd2,layers-dot:ebd3,layers-active:ebd4,compass:ebd5,compass-dot:ebd6,compass-active:ebd7,azure:ebd8,issue-draft:ebd9,git-pull-request-closed:ebda,git-pull-request-draft:ebdb,debug-all:ebdc,debug-coverage:ebdd,run-errors:ebde,folder-library:ebdf,debug-continue-small:ebe0,beaker-stop:ebe1,graph-line:ebe2,graph-scatter:ebe3,pie-chart:ebe4,bracket:eb0f,bracket-dot:ebe5,bracket-error:ebe6,lock-small:ebe7,azure-devops:ebe8,verified-filled:ebe9,newline:ebea,layout:ebeb,layout-activitybar-left:ebec',
  'layout-activitybar-right:ebed,layout-panel-left:ebee,layout-panel-center:ebef,layout-panel-justify:ebf0,layout-panel-right:ebf1,layout-panel:ebf2,layout-sidebar-left:ebf3,layout-sidebar-right:ebf4,layout-statusbar:ebf5,layout-menubar:ebf6,layout-centered:ebf7,target:ebf8,indent:ebf9,record-small:ebfa,error-small:ebfb,arrow-circle-down:ebfc,arrow-circle-left:ebfd,arrow-circle-right:ebfe,arrow-circle-up:ebff,layout-sidebar-right-off:ec00,layout-panel-off:ec01,layout-sidebar-left-off:ec02,blank:ec03,heart-filled:ec04,map:ec05,map-filled:ec06,circle-small:ec07,bell-slash:ec08,bell-slash-dot:ec09,comment-unresolved:ec0a,git-pull-request-go-to-changes:ec0b,git-pull-request-new-changes:ec0c',
].join(',');
const codicons = new Map(CODICONS.split(',').map((x) => {
  const [n, h] = x.split(':');
  return [n, parseInt(h, 16)];
}));

function iconChar (name) {
  const cp = codicons.get(String(name || '').replace(/~[\w-]+$/, ''));	// "sync~spin": sync
  return cp ? String.fromCodePoint(cp) : '';
}

// a label with $(icon)s: the glyphs in their place ("$(zap) Demo": "<zap> Demo")
function iconText (s) {
  return String(s || '').replace(/\$\(([\w-]+(?:~[\w-]+)?)\)/g, (m, n) => iconChar(n));
}


// ----------------------------------------------------------------- when clauses (menus)

// the context keys an extension's menus ask about: view, viewItem, its setContext's, config.*
function whenHolds (expr, ctx) {
  if (!expr) return true;
  const toks = [];
  const re = /\s*(&&|\|\||==|!=|=~|<=|>=|<|>|!|\(|\)|'[^']*'|"[^"]*"|\/(?:\\\/|[^/])+\/[a-z]*|[^\s&|=!<>()]+)/gy;
  let m;
  while ((m = re.exec(expr)) && m[1] !== undefined) toks.push(m[1]);
  let i = 0;
  const value = (k) => {
    if (k === 'true') return true;
    if (k === 'false') return false;
    if (/^'.*'$|^".*"$/.test(k)) return k.slice(1, -1);
    if (/^-?\d+(\.\d+)?$/.test(k)) return Number(k);
    if (ctx && Object.prototype.hasOwnProperty.call(ctx, k)) return ctx[k];
    if (Object.prototype.hasOwnProperty.call(contextKeys, k)) return contextKeys[k];
    if (k.startsWith('config.')) return settingValue(k.slice(7));
    if (k === 'isWindows') return isWin;
    if (k === 'isLinux') return process.platform === 'linux';
    if (k === 'isMac') return process.platform === 'darwin';
    return undefined;
  };
  const term = () => {
    const t = toks[i++];
    if (t === '!') return !term();
    if (t === '(') {
      const r = or();
      if (toks[i] === ')') i++;
      return r;
    }
    const v = value(t);
    const op = toks[i];
    if (op === '==' || op === '!=') {	// its right side is a literal: "view == my.view"
      i++;
      const r = toks[i++] || '';
      const w = /^'.*'$|^".*"$/.test(r) ? r.slice(1, -1) : r;
      const eq = String(v) === String(w);
      return op === '==' ? eq : !eq;
    }
    if (op === '=~') {
      i++;
      const r = /^\/(.*)\/([a-z]*)$/.exec(toks[i++] || '');
      try {
        return !!r && new RegExp(r[1], r[2]).test(String(v === undefined ? '' : v));
      } catch (e) {
        return false;
      }
    }
    if (op === '<' || op === '>' || op === '<=' || op === '>=') {
      i++;
      const a = Number(v), b = Number(value(toks[i++]));
      return op === '<' ? a < b : op === '>' ? a > b : op === '<=' ? a <= b : a >= b;
    }
    if (op === 'in' || (op === 'not' && toks[i + 1] === 'in')) {
      const neg = op === 'not';
      i += neg ? 2 : 1;
      const list = value(toks[i++]);
      const has = Array.isArray(list) ? list.includes(v) : list && typeof list === 'object' ? v in list : false;
      return neg ? !has : has;
    }
    return !!v;
  };
  const and = () => {
    let r = term();
    while (toks[i] === '&&') {
      i++;
      r = term() && r;
    }
    return r;
  };
  const or = () => {
    let r = and();
    while (toks[i] === '||') {
      i++;
      r = and() || r;
    }
    return r;
  };
  try {
    return !!or();
  } catch (e) {
    return false;
  }
}

// a contributed command's title and icon, for a menu
function commandInfo (id) {
  for (const e of exts)
    for (const c of (e.pkg.contributes && e.pkg.contributes.commands) || [])
      if (c.command === id) {
        const t = typeof c.title === 'object' ? c.title.value : c.title;
        const cat = typeof c.category === 'object' ? c.category.value : c.category;
        let icon = '';
        if (typeof c.icon === 'string') icon = iconText(c.icon);
        return {title: l10nString(e, t) || id, category: l10nString(e, cat) || '', icon};
      }
  return {title: id, category: '', icon: ''};
}

// the entries of a menu ("view/title", "view/item/context") whose when holds: {command, title, icon, inline}
function menuActions (menu, ctx) {
  const out = [];
  for (const e of exts) {
    const list = (e.pkg.contributes && e.pkg.contributes.menus && e.pkg.contributes.menus[menu]) || [];
    for (const it of list) {
      if (!it.command || !whenHolds(it.when, ctx)) continue;
      const info = commandInfo(it.command);
      out.push({command: it.command, title: info.title, icon: info.icon, inline: /^inline/.test(it.group || '')});
    }
  }
  return out;
}


// ----------------------------------------------------------------- tree views: in mme's side bar

// every tree view the extensions contribute (not a webview): the side bar's sections
const builtinContainers = {explorer: 'Explorer', scm: 'Source Control', debug: 'Run and Debug', test: 'Testing'};
function treeViewList () {
  const out = [];
  for (const e of exts) {
    const c = e.pkg.contributes || {};
    const titles = {};
    for (const list of Object.values(c.viewsContainers || {}))
      for (const vc of list || []) titles[vc.id] = l10nString(e, vc.title);
    for (const [container, list] of Object.entries(c.views || {}))
      for (const v of list || [])
        if (v.id && v.type !== 'webview')
          out.push({id: v.id, name: l10nString(e, v.name) || v.id, container,
            title: titles[container] || builtinContainers[container] || l10nString(e, e.pkg.displayName) || e.id, ext: e.id,
            when: v.when || ''});
  }
  return out;
}

const trees = new Map();	// view id -> {provider, view, byHandle, items}

function treeItemLabel (it, el) {
  if (it.label && typeof it.label === 'object') return String(it.label.label || '');
  if (it.label !== undefined) return String(it.label);
  if (it.resourceUri) return path.basename(toUri(it.resourceUri).fsPath);
  return typeof el === 'string' ? el : '';
}

function createTreeView (viewId, options) {
  const provider = options && options.treeDataProvider;
  const onExpand = new EventEmitter(), onCollapse = new EventEmitter(), onSel = new EventEmitter();
  const onVis = new EventEmitter(), onCheck = new EventEmitter();
  let message = '', title, description, badge;
  const info = () => notify('mme/treeInfo', {view: viewId, message: message || '', title: title || '', description: description || '',
    badge: badge && badge.value ? badge.value : 0});
  const t = {provider, byHandle: new Map(), handleOf: new Map(), items: new Map(), expanded: new Set(), visible: false};
  const view = {
    selection: [], visible: false,
    get message () {
      return message;
    },
    set message (m) {
      message = typeof m === 'object' && m ? m.value : m;
      info();
    },
    get title () {
      return title;
    },
    set title (s) {
      title = s;
      info();
    },
    get description () {
      return description;
    },
    set description (s) {
      description = s;
      info();
    },
    get badge () {
      return badge;
    },
    set badge (b) {
      badge = b;
      info();
    },
    onDidExpandElement: onExpand.event, onDidCollapseElement: onCollapse.event,
    onDidChangeSelection: onSel.event, onDidChangeVisibility: onVis.event, onDidChangeCheckboxState: onCheck.event,
    async reveal (el, o) {	// its parents opened, it selected, as far as getParent says
      if (!provider || typeof provider.getParent !== 'function') return;
      const chain = [];
      let p = el;
      for (let n = 0; n < 50 && p !== undefined && p !== null; n++) {
        chain.unshift(p);
        p = await provider.getParent(p);
      }
      let parent = null;
      for (const x of chain) {
        await treeChildren(viewId, parent);
        const h = t.handleOf.get(x);
        if (h === undefined) return;
        if (x !== el || (o && o.expand)) {
          t.expanded.add(h);
          await treeChildren(viewId, h);
        }
        parent = h;
      }
      notify('mme/treeReveal', {view: viewId, handle: t.handleOf.get(el), select: !o || o.select !== false});
    },
    dispose () {
      trees.delete(viewId);
    },
  };
  t.view = view;
  t.fire = {expand: onExpand, collapse: onCollapse, sel: onSel, vis: onVis};
  trees.set(viewId, t);
  if (provider && typeof provider.onDidChangeTreeData === 'function')
    provider.onDidChangeTreeData((e) => {
      const list = Array.isArray(e) ? e : [e];
      for (const x of list) {
        const h = x === undefined || x === null ? null : t.handleOf.get(x);
        if (h === undefined) continue;	// not shown: nothing to redo
        notify('mme/treeRefresh', {view: viewId, handle: h});
      }
    });
  notify('mme/treeReady', {view: viewId});	// mme asks for what it shows
  return view;
}

function registerTreeDataProvider (viewId, provider) {
  const v = createTreeView(viewId, {treeDataProvider: provider});
  return new Disposable(() => v.dispose());
}

// the children of handle (null: the view's own) as mme draws them: mme/treeItems
async function treeChildren (viewId, handle) {
  let t = trees.get(viewId);
  if (!t) {
    await activateOn('onView:' + viewId);
    t = trees.get(viewId);
  }
  if (!t || !t.provider) {
    notify('mme/treeItems', {view: viewId, parent: handle, items: [], message: 'There is no data provider registered that can provide view data.'});
    return;
  }
  const el = handle === null || handle === undefined ? undefined : t.byHandle.get(handle);
  if (handle !== null && handle !== undefined && el === undefined) return;	// gone since
  let kids = [];
  try {
    kids = (await t.provider.getChildren(el)) || [];
  } catch (e) {
    log('[error] ' + viewId + ': getChildren: ' + (e && e.stack ? e.stack : e));
  }
  const items = [], seen = new Set();
  for (const k of kids) {
    let it;
    try {
      it = await t.provider.getTreeItem(k);
    } catch (e) {
      continue;
    }
    if (!it) continue;
    const label = treeItemLabel(it, k);
    let h = it.id !== undefined ? 'id:' + it.id : (handle || '') + '/' + label;
    for (let n = 2; seen.has(h); n++) h = (it.id !== undefined ? 'id:' + it.id : (handle || '') + '/' + label) + '#' + n;
    seen.add(h);
    t.byHandle.set(h, k);
    t.handleOf.set(k, h);
    t.items.set(h, it);
    let icon = '', file = '';
    if (it.iconPath instanceof ThemeIcon || (it.iconPath && it.iconPath.id && !it.iconPath.scheme)) icon = iconChar(it.iconPath.id);
    const res = it.resourceUri ? toUri(it.resourceUri).fsPath : '';
    if (!icon && res) file = res;	// mme gives it the file's icon
    let desc = it.description;
    if (desc === true) desc = res ? path.dirname(asRelativePath(res)) : '';
    const tip = typeof it.tooltip === 'string' ? it.tooltip : it.tooltip && it.tooltip.value ? it.tooltip.value : '';
    const coll = it.collapsibleState || 0;
    if (coll === 2) t.expanded.add(h);
    items.push({handle: h, label: iconText(label), description: desc ? iconText(String(desc)) : '', tooltip: tip, icon, file,
      folder: !!(res && coll), collapsible: t.expanded.has(h) ? 2 : coll, command: !!it.command,
      actions: menuActions('view/item/context', {view: viewId, viewItem: it.contextValue || ''})});
  }
  notify('mme/treeItems', {view: viewId, parent: handle === undefined ? null : handle, items,
    message: t.view.message || '', titleActions: handle === null || handle === undefined ? menuActions('view/title', {view: viewId}) : undefined});
}

async function treeExpand (p) {	// mme opened (or closed) a node, or a view (handle null)
  const t = trees.get(p.view);
  const h = p.handle === undefined ? null : p.handle;
  if (h === null) {
    if (t && t.visible !== !!p.expanded) {
      t.visible = t.view.visible = !!p.expanded;
      t.fire.vis.fire({visible: t.visible});
    }
    if (p.expanded) await treeChildren(p.view, null);
    return;
  }
  if (!t) return;
  const el = t.byHandle.get(h);
  if (el === undefined) return;
  if (p.expanded) {
    t.expanded.add(h);
    t.fire.expand.fire({element: el});
    await treeChildren(p.view, h);
  } else {
    t.expanded.delete(h);
    t.fire.collapse.fire({element: el});
  }
}

async function treeSelect (p) {	// a row chosen: selection, and the item's command
  const t = trees.get(p.view);
  if (!t) return;
  const el = t.byHandle.get(p.handle), it = t.items.get(p.handle);
  if (el === undefined) return;
  t.view.selection = [el];
  t.fire.sel.fire({selection: [el]});
  if (it && it.command && p.run) {
    const c = it.command;
    try {
      await (typeof c === 'string' ? executeCommand(c) : executeCommand(c.command, ...(c.arguments || [])));
    } catch (e) {
      log('[error] ' + (c.command || c) + ': ' + (e && e.message ? e.message : e));
    }
  }
}

async function treeAction (p) {	// a menu's command on a row (its element) or on the view's title
  const t = trees.get(p.view);
  const el = t && p.handle !== null && p.handle !== undefined ? t.byHandle.get(p.handle) : undefined;
  try {
    if (el !== undefined) await executeCommand(p.command, el, t.view.selection.length ? t.view.selection : [el]);
    else await executeCommand(p.command);
  } catch (e) {
    log('[error] ' + p.command + ': ' + (e && e.message ? e.message : e));
    window.showErrorMessage(String(e && e.message ? e.message : e));
  }
}


// ----------------------------------------------------------------- tasks: the extensions' in mme's Run Task

// registerTaskProvider's tasks go to mme (mme/tasks) as command lines it runs in its task terminal, with its
// problem matchers; a CustomExecution (the extension writes the terminal itself) runs here, its output in
// an Output channel. mme asks for them when Run Task opens (mme/provideTasks).
const taskProviders = new Map();	// type -> provider
const providedTasks = new Map();	// id -> Task, of the last mme/tasks
let nextTaskId = 1;
const onDidStartTask = new EventEmitter(), onDidEndTask = new EventEmitter();
const onDidStartTaskProcess = new EventEmitter(), onDidEndTaskProcess = new EventEmitter();
const taskExecutions = [];

function shellQuote (a) {
  const s = typeof a === 'object' && a ? String(a.value) : String(a);
  if (s === '' || /[\s"'&|<>^()]/.test(s)) return isWin ? '"' + s.replace(/"/g, '\\"') + '"' : "'" + s.replace(/'/g, "'\\''") + "'";
  return s;
}

// a task as mme runs it: {id, label, source, cmd, cwd, group, matcher, custom}
function taskToMme (t, id) {
  const ex = t.execution || t.__execution;
  let cmd = '', cwd = '', custom = false;
  if (ex instanceof ShellExecution) {
    cmd = ex.commandLine !== undefined ? String(ex.commandLine) : [ex.command, ...(ex.args || [])].map(shellQuote).join(' ');
    cwd = ex.options && ex.options.cwd;
  } else if (ex instanceof ProcessExecution) {
    cmd = [ex.process, ...(ex.args || [])].map(shellQuote).join(' ');
    cwd = ex.options && ex.options.cwd;
  } else if (ex instanceof CustomExecution) custom = true;
  else return null;
  const folder = t.scope && t.scope.uri ? t.scope.uri.fsPath : folders[0] ? folders[0].uri.fsPath : '';
  const pm = [].concat(t.problemMatchers || []).map(String);
  const matcher = pm.find((m) => /gcc|go|tsc|msCompile/.test(m)) || '';
  const g = t.group && t.group.id ? t.group.id : '';
  const label = (t.source ? t.source + ': ' : '') + t.name;
  return {id, label, source: t.source || (t.definition && t.definition.type) || '', detail: t.detail || '', cmd,
    cwd: cwd ? String(cwd).replace(/\$\{workspaceFolder\}/g, folder) : folder, group: g, dflt: !!(t.group && t.group.isDefault),
    matcher, custom};
}

async function collectTasks (type) {
  const out = [];
  for (const [ty, p] of taskProviders) {
    if (type && ty !== type) continue;
    try {
      const list = (await p.provideTasks(new CancellationTokenSource().token)) || [];
      for (const t of list) out.push(t);
    } catch (e) {
      log('[error] task provider ' + ty + ': ' + (e && e.stack ? e.stack : e));
    }
  }
  return out;
}

async function provideTasksToMme () {	// mme/provideTasks: Run Task is opening
  await activateOn('onCommand:workbench.action.tasks.runTask');
  for (const e of exts)
    for (const d of (e.pkg.contributes && e.pkg.contributes.taskDefinitions) || [])
      if (d.type) await activateOn('onTaskType:' + d.type);
  const list = await collectTasks();
  providedTasks.clear();
  const items = [];
  for (const t of list) {
    const id = 't' + nextTaskId++;
    const m = taskToMme(t, id);
    if (!m) continue;
    providedTasks.set(id, t);
    items.push(m);
  }
  notify('mme/tasks', {tasks: items});
}

// a CustomExecution: its Pseudoterminal's output in an Output channel named after the task
async function runCustom (t, label) {
  const ch = createOutputChannel('Task - ' + label);
  ch.show(true);
  let term;
  try {
    term = await t.execution.callback(t.definition || {});
  } catch (e) {
    ch.appendLine('[error] ' + (e && e.message ? e.message : e));
    return;
  }
  const exec = {task: t, terminate: () => term && term.close && term.close()};
  taskExecutions.push(exec);
  onDidStartTask.fire({execution: exec});
  const done = (code) => {
    const i = taskExecutions.indexOf(exec);
    if (i >= 0) taskExecutions.splice(i, 1);
    ch.appendLine('');
    ch.appendLine('* The task finished' + (typeof code === 'number' ? ' with exit code ' + code : '') + '.');
    onDidEndTask.fire({execution: exec});
    onDidEndTaskProcess.fire({execution: exec, exitCode: code});
  };
  if (term.onDidWrite) term.onDidWrite((s) => ch.append(String(s).replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '').replace(/\r\n/g, '\n')));
  if (term.onDidClose) term.onDidClose((code) => done(code));
  try {
    term.open({columns: 120, rows: 30});
  } catch (e) {
    ch.appendLine('[error] ' + (e && e.message ? e.message : e));
    done(1);
  }
}

async function runTaskFromMme (p) {	// mme/runTask {id}: a task mme cannot run itself (CustomExecution)
  const t = providedTasks.get(p.id);
  if (t) await runCustom(t, p.label || t.name);
}

async function executeTask (t) {	// tasks.executeTask: mme runs it as one of its own
  if (!t.execution && t.__execution) t.execution = t.__execution;
  const m = taskToMme(t, 't' + nextTaskId++);
  if (!m) throw new Error('mme runs shell, process and custom tasks only');
  if (m.custom) {
    providedTasks.set(m.id, t);
    runCustom(t, m.label);
  } else notify('mme/runTask', m);
  const exec = {task: t, terminate () {}};
  onDidStartTask.fire({execution: exec});
  return exec;
}


// ----------------------------------------------------------------- tests: the extensions' in mme's Testing view

// tests.createTestController: its items (a tree: package, file, test, subtest) go to mme flattened, a test
// being an item with a place in a file (mme/tests); mme runs them through the controller's run profile
// (mme/testRun) and the TestRun's states come back as mme/testState, its output as mme/testOutput.
const testControllers = new Map();	// id -> controller
const testById = new Map();	// "<controller>/<item id>" -> item, of the last mme/tests
let testSendTimer = null;

function testsChanged () {
  if (testSendTimer) return;
  testSendTimer = setTimeout(() => {
    testSendTimer = null;
    sendTests();
  }, 150);
}

class TestItemCollection {
  constructor (owner) {
    this._m = new Map();
    this._owner = owner;	// the item whose children these are; null: a controller's items
  }
  get size () {
    return this._m.size;
  }
  replace (items) {
    this._m.clear();
    for (const it of items || []) this._add(it);
    testsChanged();
  }
  forEach (cb, thisArg) {
    for (const it of [...this._m.values()]) cb.call(thisArg, it, this);
  }
  add (it) {
    this._add(it);
    testsChanged();
  }
  _add (it) {
    it.parent = this._owner || undefined;
    this._m.set(it.id, it);
  }
  delete (id) {
    this._m.delete(id);
    testsChanged();
  }
  get (id) {
    return this._m.get(id);
  }
  [Symbol.iterator] () {
    return this._m.entries();
  }
}

class TestItem {
  constructor (ctrl, id, label, uri) {
    this.controller = ctrl;
    this.id = id;
    this.uri = uri;
    this.children = new TestItemCollection(this);
    this.parent = undefined;
    this.tags = [];
    this._label = label;
    this.description = undefined;
    this.sortText = undefined;
    this._range = undefined;
    this.error = undefined;
    this.busy = false;
    this.canResolveChildren = false;
  }
  get label () {
    return this._label;
  }
  set label (s) {
    this._label = s;
    testsChanged();
  }
  get range () {
    return this._range;
  }
  set range (r) {
    this._range = r;
    testsChanged();
  }
}

class TestRunRequest {
  constructor (include, exclude, profile, continuous, preserveFocus) {
    Object.assign(this, {include, exclude, profile, continuous: !!continuous, preserveFocus: preserveFocus !== false});
  }
}

class TestMessage {
  constructor (message) {
    this.message = message;
  }
  static diff (message, expected, actual) {
    const m = new TestMessage(message);
    m.expectedOutput = expected;
    m.actualOutput = actual;
    return m;
  }
}

class TestTag {
  constructor (id) {
    this.id = id;
  }
}

function testGid (it) {
  return it.controller.id + '/' + it.id;
}

// every test of every controller, as mme lists them: {id, label, file, line}
function sendTests () {
  const out = [];
  testById.clear();
  const walk = (it, names) => {
    const file = it.uri && it.uri.scheme === 'file' ? it.uri.fsPath : '';
    const isTest = !!it.range;	// a place in a file: a test (a package, a folder, a file is not)
    const label = String(it.label || it.id);
    const here = isTest ? [...names, label] : names;
    if (isTest) {
      const gid = testGid(it);
      testById.set(gid, it);
      out.push({id: gid, label: here.join('/'), file, line: it.range.start.line, controller: it.controller.label || it.controller.id});
    }
    it.children.forEach((c) => walk(c, here));
  };
  for (const c of testControllers.values()) c.items.forEach((it) => walk(it, []));
  notify('mme/tests', {tests: out});
}

let testsDiscovered = false;
async function discoverTests () {	// mme/testDiscover: the Testing view is shown; every controller looks
  testsDiscovered = true;
  for (const c of testControllers.values()) await resolveAll(c);
  sendTests();
}

async function resolveAll (c) {
  if (typeof c.resolveHandler !== 'function') return;
  try {
    await c.resolveHandler(undefined);
  } catch (e) {
    log('[error] ' + c.id + ': resolveHandler: ' + (e && e.message ? e.message : e));
  }
  const queue = [];
  c.items.forEach((it) => queue.push(it));
  let n = 0;
  while (queue.length && n < 400) {	// what can be resolved, resolved: the files' tests come
    const it = queue.shift();
    if (it.canResolveChildren && !it._resolved) {
      it._resolved = true;
      n++;
      try {
        await c.resolveHandler(it);
      } catch (e) {
        // an item it cannot open: its children stay unknown
      }
    }
    it.children.forEach((k) => queue.push(k));
  }
}

function createTestController (id, label) {
  const c = {
    id, label,
    items: new TestItemCollection(null),
    resolveHandler: undefined, refreshHandler: undefined,
    _profiles: [],
    createTestItem: (tid, tlabel, uri) => new TestItem(c, tid, tlabel, uri),
    createRunProfile (plabel, kind, runHandler, isDefault, tag, supportsContinuousRun) {
      const p = {label: plabel, kind, runHandler, isDefault: !!isDefault, tag, supportsContinuousRun: !!supportsContinuousRun,
        configureHandler: undefined, loadDetailedCoverage: undefined, onDidChangeDefault: stubEvent(),
        dispose () {
          const i = c._profiles.indexOf(p);
          if (i >= 0) c._profiles.splice(i, 1);
        }};
      c._profiles.push(p);
      return p;
    },
    createTestRun (request, name, persist) {
      return createTestRun(c, request, name, persist);
    },
    invalidateTestResults () {},
    dispose () {
      testControllers.delete(id);
      testsChanged();
    },
  };
  testControllers.set(id, c);
  if (testsDiscovered) setTimeout(() => resolveAll(c).then(sendTests), 0);
  return c;
}

function messageText (m) {
  const one = (x) => (x && typeof x === 'object' ? (x.message && x.message.value !== undefined ? x.message.value : x.message) : x);
  return (Array.isArray(m) ? m.map(one) : [one(m)]).filter((x) => x !== undefined && x !== null).map(String).join('\n');
}

function messagePlace (m) {
  const x = Array.isArray(m) ? m.find((y) => y && y.location) : m;
  const loc = x && x.location;
  return loc && loc.uri ? {file: loc.uri.fsPath, line: loc.range ? loc.range.start.line : 0} : {};
}

function createTestRun (c, request, name, persist) {
  const cts = new CancellationTokenSource();
  const state = (it, s, extra) => {
    if (!it) return;
    notify('mme/testState', Object.assign({id: testGid(it), state: s}, extra || {}));
  };
  const onDispose = new EventEmitter();
  const run = {
    name, token: cts.token, isPersisted: persist !== false,
    enqueued: (it) => state(it, 'queued'),
    started: (it) => state(it, 'started'),
    skipped: (it) => state(it, 'skipped'),
    passed: (it, ms) => state(it, 'passed', {ms: typeof ms === 'number' ? Math.round(ms) : -1}),
    failed: (it, m, ms) => state(it, 'failed', Object.assign({ms: typeof ms === 'number' ? Math.round(ms) : -1, message: messageText(m)}, messagePlace(m))),
    errored: (it, m, ms) => state(it, 'failed', Object.assign({ms: typeof ms === 'number' ? Math.round(ms) : -1, message: messageText(m)}, messagePlace(m))),
    appendOutput: (text) => notify('mme/testOutput', {text: String(text).replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '').replace(/\r\n/g, '\n')}),
    addCoverage () {},
    onDidDispose: onDispose.event,
    end () {
      notify('mme/testEnd', {});
      onDispose.fire();
    },
  };
  activeTestRuns.push({run, cts});
  return run;
}
const activeTestRuns = [];

async function runTestsFromMme (p) {	// mme/testRun {ids, kind}: each controller's default profile of that kind
  const kind = p.kind === 'debug' ? 2 : p.kind === 'coverage' ? 3 : 1;
  const byCtrl = new Map();
  for (const id of p.ids || []) {
    const it = testById.get(id);
    if (!it) continue;
    if (!byCtrl.has(it.controller)) byCtrl.set(it.controller, []);
    byCtrl.get(it.controller).push(it);
  }
  for (const [c, items] of byCtrl) {
    const profile = c._profiles.find((x) => x.kind === kind && x.isDefault) || c._profiles.find((x) => x.kind === kind);
    if (!profile) {
      window.showWarningMessage(c.label + ': it cannot ' + (kind === 2 ? 'debug' : kind === 3 ? 'measure coverage of' : 'run') + ' tests');
      notify('mme/testEnd', {});
      continue;
    }
    const cts = new CancellationTokenSource();
    activeTestRuns.push({run: null, cts});
    try {
      await profile.runHandler(new TestRunRequest(items, [], profile), cts.token);
    } catch (e) {
      log('[error] ' + c.id + ': run: ' + (e && e.stack ? e.stack : e));
      notify('mme/testEnd', {});
    }
  }
}

function cancelTestRuns () {	// mme/testCancel
  for (const r of activeTestRuns.splice(0)) r.cts.cancel();
}


// ----------------------------------------------------------------- debug adapters: the extensions' in mme's debugger

// A launch configuration of a type an extension contributes (contributes.debuggers) goes through it: mme sends
// mme/debugResolve, its providers' resolveDebugConfiguration change it, its adapter factory says how to reach the
// adapter - a program mme starts, a port mme connects to, or one written in JavaScript (an inline adapter),
// which gets a port here that carries its messages. mme is the DAP client as for its own adapters.
const net = require('net');
const debugConfigProviders = [];	// {type, provider, trigger}
const debugAdapterFactories = new Map();	// type -> factory
const onDidStartDebugSession = new EventEmitter(), onDidTerminateDebugSession = new EventEmitter();
const onDidChangeActiveDebugSession = new EventEmitter(), onDidReceiveDebugSessionCustomEvent = new EventEmitter();
const onDidChangeBreakpoints = new EventEmitter();
let activeDebugSession, debugBridge = null;
const debugTrackerFactories = [];	// {type, factory}
let debugTrackers = [];	// the session's trackers
const debugRequests = new Map();	// customRequest's id -> {resolve, reject}
let nextDebugRequest = 1;
const DAP_EVENTS = new Set(['initialized', 'stopped', 'continued', 'exited', 'terminated', 'thread', 'output', 'breakpoint',
  'module', 'loadedSource', 'process', 'capabilities', 'progressStart', 'progressUpdate', 'progressEnd', 'invalidated',
  'memory']);

function trackersCall (method, ...a) {
  for (const t of debugTrackers)
    if (t && typeof t[method] === 'function') {
      try {
        t[method](...a);
      } catch (e) {
        log('[error] a debug adapter tracker: ' + (e && e.message ? e.message : e));
      }
    }
}

function dapOut (p) {	// mme/dapOut: a message mme sent the adapter
  trackersCall('onWillReceiveMessage', p.message);
}

function dapIn (p) {	// mme/dapIn: a message of the adapter's
  const m = p.message || {};
  trackersCall('onDidSendMessage', m);
  if (m.type === 'event' && !DAP_EVENTS.has(m.event) && activeDebugSession)
    onDidReceiveDebugSessionCustomEvent.fire({session: activeDebugSession, event: m.event, body: m.body});
}

function debugResponse (p) {	// mme/debugResponse: customRequest's answer
  const r = debugRequests.get(p.id);
  if (!r) return;
  debugRequests.delete(p.id);
  if (p.success) r.resolve(p.body);
  else r.reject(new Error(p.message || 'the debug adapter refused the request'));
}

class DebugAdapterNamedPipeServer {
  constructor (p) {
    this.path = p;
  }
}

class DebugAdapterInlineImplementation {
  constructor (implementation) {
    this.implementation = implementation;
  }
}

function debuggerTypes () {	// what the extensions contribute: {type, label, ext}
  const out = [];
  for (const e of exts)
    for (const d of (e.pkg.contributes && e.pkg.contributes.debuggers) || [])
      if (d.type) out.push({type: d.type, label: l10nString(e, d.label) || d.type, ext: e});
  return out;
}

// contributes.debuggers' own program (the old way, before factories), as an executable
function packageExecutable (type) {
  for (const e of exts)
    for (const d of (e.pkg.contributes && e.pkg.contributes.debuggers) || []) {
      if (d.type !== type) continue;
      const os = isWin ? d.windows : process.platform === 'darwin' ? d.osx : d.linux;
      const prog = (os && os.program) || d.program, runtime = (os && os.runtime) || d.runtime;
      const args = (os && os.args) || d.args || [];
      if (!prog) continue;
      const file = path.resolve(e.dir, prog);
      if (!runtime) return new DebugAdapterExecutable(file, args);
      return new DebugAdapterExecutable(runtime === 'node' ? process.execPath : runtime, [file, ...args]);
    }
  return undefined;
}

function debugSession (config) {
  const s = {
    id: crypto.randomUUID(), type: config.type, name: config.name || config.type, workspaceFolder: folders[0],
    configuration: config, parentSession: undefined,
    customRequest: (command, args) => new Promise((resolve, reject) => {	// through mme's connection to the adapter
      const id = nextDebugRequest++;
      debugRequests.set(id, {resolve, reject});
      notify('mme/debugRequest', {id, command, args: args === undefined ? {} : plain(args)});
    }),
    getDebugProtocolBreakpoint: async () => undefined,
  };
  return s;
}

// an inline adapter: a port on 127.0.0.1; mme's messages go to handleMessage, its onDidSendMessage's come back
function bridge (impl) {
  return new Promise((resolve, reject) => {
    const srv = net.createServer((sock) => {
      let buf = Buffer.alloc(0);
      const sub = impl.onDidSendMessage((m) => {
        const body = Buffer.from(JSON.stringify(m), 'utf8');
        sock.write('Content-Length: ' + body.length + '\r\n\r\n');
        sock.write(body);
      });
      sock.on('data', (d) => {
        buf = Buffer.concat([buf, d]);
        for (;;) {
          const sep = buf.indexOf('\r\n\r\n');
          if (sep < 0) return;
          const m = /Content-Length:\s*(\d+)/i.exec(buf.slice(0, sep).toString());
          const n = m ? +m[1] : 0;
          if (buf.length < sep + 4 + n) return;
          const body = buf.slice(sep + 4, sep + 4 + n).toString('utf8');
          buf = buf.slice(sep + 4 + n);
          try {
            impl.handleMessage(JSON.parse(body));
          } catch (e) {
            log('[error] the inline debug adapter: ' + (e && e.stack ? e.stack : e));
          }
        }
      });
      const end = () => {
        if (sub && sub.dispose) sub.dispose();
        if (typeof impl.dispose === 'function') impl.dispose();
        srv.close();
      };
      sock.on('close', end);
      sock.on('error', end);
    });
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => resolve({srv, port: srv.address().port}));
  });
}

async function resolveDebug (p) {	// mme/debugResolve {seq, config}: mme/debugResolved {seq, config, adapter}
  const seq = p.seq;
  const answer = (x) => notify('mme/debugResolved', Object.assign({seq}, x));
  let config = p.config || {};
  try {
    await activateOn('onDebug');
    await activateOn('onDebugResolve:' + config.type);
    await activateOn('onDebugAdapterProtocolTracker:' + config.type);
    const folder = folders[0], token = new CancellationTokenSource().token;
    for (const step of ['resolveDebugConfiguration', 'resolveDebugConfigurationWithSubstitutedVariables'])
      for (const x of debugConfigProviders) {
        if ((x.type !== config.type && x.type !== '*') || typeof x.provider[step] !== 'function') continue;
        config = await x.provider[step](folder, config, token);
        if (config === undefined || config === null) {	// the extension stopped it (it said why, or it opened launch.json)
          answer({config: null});
          return;
        }
      }
    const session = debugSession(config);
    const factory = debugAdapterFactories.get(config.type);
    const exe = packageExecutable(config.type);
    let d = factory ? await factory.createDebugAdapterDescriptor(session, exe) : exe;
    if (!d) d = exe;
    let adapter;
    if (d instanceof DebugAdapterExecutable || (d && d.command)) {
      const o = d.options || {};
      adapter = {kind: 'exec', command: d.command, args: (d.args || []).map(String), cwd: o.cwd || '', env: o.env || null};
    } else if (d instanceof DebugAdapterServer || (d && d.port)) adapter = {kind: 'server', port: d.port, host: d.host || '127.0.0.1'};
    else if (d instanceof DebugAdapterInlineImplementation || (d && d.implementation)) {
      debugBridge = await bridge(d.implementation);
      adapter = {kind: 'server', port: debugBridge.port, host: '127.0.0.1'};
    } else if (d instanceof DebugAdapterNamedPipeServer) {
      answer({error: 'a debug adapter on a named pipe is not supported yet'});
      return;
    } else {
      answer({error: 'the extension gives no debug adapter for type "' + config.type + '"'});
      return;
    }
    activeDebugSession = session;
    debugTrackers = [];
    for (const x of debugTrackerFactories)
      if (x.type === config.type || x.type === '*') {
        try {
          const t = await x.factory.createDebugAdapterTracker(session);
          if (t) debugTrackers.push(t);
        } catch (e) {
          log('[error] a debug adapter tracker factory: ' + (e && e.message ? e.message : e));
        }
      }
    trackersCall('onWillStartSession');
    // mme sends the messages here only when something wants them
    answer({config, adapter, track: debugTrackers.length > 0 || onDidReceiveDebugSessionCustomEvent._ls.length > 0});
    onDidStartDebugSession.fire(session);
    onDidChangeActiveDebugSession.fire(session);
  } catch (e) {
    log('[error] debug ' + config.type + ': ' + (e && e.stack ? e.stack : e));
    answer({error: String(e && e.message ? e.message : e)});
  }
}

function debugEnded () {	// mme/debugEnded: the session is over
  const s = activeDebugSession;
  activeDebugSession = undefined;
  trackersCall('onWillStopSession');
  trackersCall('onExit', 0, undefined);
  debugTrackers = [];
  for (const r of debugRequests.values()) r.reject(new Error('the debug session ended'));
  debugRequests.clear();
  if (debugBridge) {
    try {
      debugBridge.srv.close();
    } catch (e) {
      // closed already
    }
    debugBridge = null;
  }
  if (s) {
    onDidTerminateDebugSession.fire(s);
    onDidChangeActiveDebugSession.fire(undefined);
  }
}

async function startDebugging (folder, nameOrConfig) {	// debug.startDebugging: mme's debugger with it
  const config = typeof nameOrConfig === 'string' ? {name: nameOrConfig} : plain(nameOrConfig);
  notify('mme/startDebugging', {config});
  return true;
}

const debugConsole = {
  append: (s) => notify('mme/debugConsole', {text: String(s)}),
  appendLine: (s) => notify('mme/debugConsole', {text: String(s) + '\n'}),
};


// ----------------------------------------------------------------- language models: mme's chat model

// vscode.lm: an extension asking for a model gets the one mme's Chat talks to (mme.chat.provider, .model,
// .apiKey, .baseUrl; the same environment variables), streamed as VS Code's LanguageModelChatResponse.
// Tools (lm.registerTool) and chat participants (chat.createChatParticipant: "Ask @name..." in the palette).
const LanguageModelChatMessageRole = {User: 1, Assistant: 2};
const LanguageModelChatToolMode = {Auto: 1, Required: 2};

class LanguageModelTextPart {
  constructor (value) {
    this.value = value;
  }
}
class LanguageModelToolCallPart {
  constructor (callId, name, input) {
    Object.assign(this, {callId, name, input});
  }
}
class LanguageModelToolResultPart {
  constructor (callId, content) {
    Object.assign(this, {callId, content});
  }
}
class LanguageModelToolResult {
  constructor (content) {
    this.content = content;
  }
}
class LanguageModelPromptTsxPart {
  constructor (value) {
    this.value = value;
  }
}
class LanguageModelDataPart {
  constructor (data, mimeType) {
    Object.assign(this, {data, mimeType});
  }
  static text (s, mime) {
    return new LanguageModelDataPart(Buffer.from(String(s)), mime || 'text/plain');
  }
  static json (v, mime) {
    return new LanguageModelDataPart(Buffer.from(JSON.stringify(v)), mime || 'text/x-json');
  }
}
class LanguageModelChatMessage {
  constructor (role, content, name) {
    this.role = role;
    this.content = typeof content === 'string' ? [new LanguageModelTextPart(content)] : content || [];
    this.name = name;
  }
  static User (content, name) {
    return new LanguageModelChatMessage(1, content, name);
  }
  static Assistant (content, name) {
    return new LanguageModelChatMessage(2, content, name);
  }
}

function lmConfig () {
  const openai = settingValue('mme.chat.provider') === 'openai';
  let key, bearer = openai;
  if (openai) key = settingValue('mme.chat.openai.apiKey') || process.env.OPENAI_API_KEY || '';
  else {
    key = settingValue('mme.chat.apiKey') || process.env.ANTHROPIC_API_KEY || '';
    if (!key && process.env.ANTHROPIC_AUTH_TOKEN) {
      key = process.env.ANTHROPIC_AUTH_TOKEN;
      bearer = true;
    }
  }
  let base = openai ? settingValue('mme.chat.openai.baseUrl') || process.env.OPENAI_BASE_URL || 'https://api.openai.com/v1'
    : settingValue('mme.chat.baseUrl') || process.env.ANTHROPIC_BASE_URL || 'https://api.anthropic.com';
  base = String(base).replace(/\/+$/, '');
  const local = /:\/\/(localhost|127\.0\.0\.1|\[::1\])/.test(base);
  const model = settingValue('mme.chat.model') || (openai ? 'gpt-4o' : 'claude-opus-5');
  return {openai, key, bearer, base, model, ok: !!key || (openai && local), fallbacks: settingValue('mme.chat.fallbacks') !== false};
}

function partText (p) {
  if (typeof p === 'string') return p;
  if (p instanceof LanguageModelTextPart || (p && typeof p.value === 'string')) return p.value;
  if (p instanceof LanguageModelPromptTsxPart) return typeof p.value === 'string' ? p.value : JSON.stringify(p.value);
  if (p instanceof LanguageModelDataPart) return p.data.toString();
  return '';
}

// the messages as the API wants them: Anthropic's blocks, or OpenAI's roles
function lmMessages (messages, openai) {
  const out = [];
  for (const m of messages) {
    const role = m.role === 2 ? 'assistant' : 'user';
    const parts = typeof m.content === 'string' ? [new LanguageModelTextPart(m.content)] : m.content || [];
    if (openai) {
      const text = parts.filter((p) => !(p instanceof LanguageModelToolCallPart) && !(p instanceof LanguageModelToolResultPart)).map(partText).join('');
      const calls = parts.filter((p) => p instanceof LanguageModelToolCallPart);
      const results = parts.filter((p) => p instanceof LanguageModelToolResultPart);
      for (const r of results) out.push({role: 'tool', tool_call_id: r.callId, content: (r.content || []).map(partText).join('')});
      if (text || calls.length)
        out.push(Object.assign({role, content: text || null}, calls.length ? {tool_calls: calls.map((c) => ({id: c.callId, type: 'function',
          function: {name: c.name, arguments: JSON.stringify(c.input || {})}}))} : {}));
      continue;
    }
    const blocks = [];
    for (const p of parts) {
      if (p instanceof LanguageModelToolCallPart) blocks.push({type: 'tool_use', id: p.callId, name: p.name, input: p.input || {}});
      else if (p instanceof LanguageModelToolResultPart) blocks.push({type: 'tool_result', tool_use_id: p.callId, content: (p.content || []).map(partText).join('')});
      else {
        const t = partText(p);
        if (t) blocks.push({type: 'text', text: t});
      }
    }
    if (!blocks.length) continue;
    const last = out[out.length - 1];
    if (last && last.role === role) last.content.push(...blocks);	// the API wants user and assistant in turn
    else out.push({role, content: blocks});
  }
  if (!openai && out.length && out[0].role === 'assistant') out.unshift({role: 'user', content: [{type: 'text', text: '(start)'}]});
  return out;
}

// a response whose parts come as the stream does; stream and text can each be read (once or more)
function lmResponse () {
  const parts = [];
  let done = false, error = null;
  const waiters = [];
  const wake = () => waiters.splice(0).forEach((w) => w());
  const iter = (map) => ({
    [Symbol.asyncIterator] () {
      let i = 0;
      return {
        async next () {
          for (;;) {
            while (i < parts.length) {
              const v = map(parts[i++]);
              if (v !== undefined) return {value: v, done: false};
            }
            if (error) throw error;
            if (done) return {value: undefined, done: true};
            await new Promise((r) => waiters.push(r));
          }
        },
      };
    },
  });
  return {
    push (p) {
      parts.push(p);
      wake();
    },
    end (e) {
      done = true;
      error = e || null;
      wake();
    },
    response: {
      stream: iter((p) => p),
      text: iter((p) => (p instanceof LanguageModelTextPart ? p.value : undefined)),
    },
  };
}

async function lmSend (messages, options, token) {
  const c = lmConfig();
  if (!c.ok) throw LanguageModelError.NoPermissions('No model: set Chat\'s API key (Chat: Set API Key...) in mme');
  const o = options || {};
  const tools = (o.tools || []).map((t) => ({name: t.name, description: t.description || '', schema: t.inputSchema || {type: 'object', properties: {}}}));
  let url, headers, body;
  const system = messages.filter((m) => m.role === 3).map((m) => (m.content || []).map(partText).join('')).join('\n') ||
    'You are a helpful assistant, asked by a Visual Studio Code extension running in the mme code editor.';
  if (c.openai) {
    url = c.base + '/chat/completions';
    headers = {'content-type': 'application/json'};
    if (c.key) headers.authorization = 'Bearer ' + c.key;
    body = {model: c.model, stream: true, messages: [{role: 'system', content: system}, ...lmMessages(messages, true)]};
    if (tools.length) body.tools = tools.map((t) => ({type: 'function', function: {name: t.name, description: t.description, parameters: t.schema}}));
  } else {
    url = c.base + '/v1/messages';
    headers = {'content-type': 'application/json', 'anthropic-version': '2023-06-01'};
    if (c.bearer) headers.authorization = 'Bearer ' + c.key;
    else headers['x-api-key'] = c.key;
    if (c.fallbacks) headers['anthropic-beta'] = 'server-side-fallback-2026-07-01';
    body = {model: c.model, max_tokens: 64000, stream: true, system, messages: lmMessages(messages, false)};
    if (c.fallbacks) body.fallbacks = 'default';
    if (tools.length) body.tools = tools.map((t) => ({name: t.name, description: t.description, input_schema: t.schema}));
    if (tools.length && o.toolMode === 2) body.tool_choice = {type: 'any'};
  }
  const ac = new AbortController();
  if (token && token.onCancellationRequested) token.onCancellationRequested(() => ac.abort());
  const res = await fetch(url, {method: 'POST', headers, body: JSON.stringify(body), signal: ac.signal});
  if (!res.ok) {
    let msg = res.status + ' ' + res.statusText;
    try {
      const j = await res.json();
      msg = (j.error && (j.error.message || j.error)) || msg;
    } catch (e) {
      // not JSON
    }
    throw res.status === 401 || res.status === 403 ? LanguageModelError.NoPermissions(String(msg)) : new LanguageModelError(String(msg));
  }
  const out = lmResponse();
  (async () => {
    const dec = new TextDecoder();
    const reader = res.body.getReader();
    let buf = '';
    const calls = new Map();	// index -> {id, name, json}
    const line = (data) => {
      if (!data || data === '[DONE]') return;
      let j;
      try {
        j = JSON.parse(data);
      } catch (e) {
        return;
      }
      if (c.openai) {
        const d = j.choices && j.choices[0] && j.choices[0].delta;
        if (!d) return;
        if (d.content) out.push(new LanguageModelTextPart(d.content));
        for (const tc of d.tool_calls || []) {
          const k = calls.get(tc.index) || {id: '', name: '', json: ''};
          if (tc.id) k.id = tc.id;
          if (tc.function && tc.function.name) k.name = tc.function.name;
          if (tc.function && tc.function.arguments) k.json += tc.function.arguments;
          calls.set(tc.index, k);
        }
        return;
      }
      if (j.type === 'content_block_start' && j.content_block && j.content_block.type === 'tool_use')
        calls.set(j.index, {id: j.content_block.id, name: j.content_block.name, json: ''});
      else if (j.type === 'content_block_delta' && j.delta) {
        if (j.delta.type === 'text_delta') out.push(new LanguageModelTextPart(j.delta.text));
        else if (j.delta.type === 'input_json_delta' && calls.has(j.index)) calls.get(j.index).json += j.delta.partial_json;
      } else if (j.type === 'error') throw new LanguageModelError(j.error && j.error.message ? j.error.message : 'error');
    };
    try {
      for (;;) {
        const {value, done} = await reader.read();
        if (done) break;
        buf += dec.decode(value, {stream: true});
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
          const l = buf.slice(0, i).replace(/\r$/, '');
          buf = buf.slice(i + 1);
          if (l.startsWith('data:')) line(l.slice(5).trim());
        }
      }
      for (const k of calls.values()) {
        let input = {};
        try {
          input = k.json ? JSON.parse(k.json) : {};
        } catch (e) {
          input = {};
        }
        out.push(new LanguageModelToolCallPart(k.id, k.name, input));
      }
      out.end();
    } catch (e) {
      out.end(e instanceof LanguageModelError ? e : new LanguageModelError(String(e && e.message ? e.message : e)));
    }
  })();
  return out.response;
}

function lmModel () {
  const c = lmConfig();
  return {
    id: 'mme-' + c.model, name: c.model + ' (mme Chat)', vendor: c.openai ? 'openai' : 'anthropic', family: c.model,
    version: c.model, maxInputTokens: 200000,
    sendRequest: (messages, options, token) => lmSend(messages, options, token),
    countTokens: async (x) => Math.ceil((typeof x === 'string' ? x : (x.content || []).map(partText).join('')).length / 4),
  };
}

const onDidChangeChatModels = new EventEmitter();
const lmTools = new Map();	// name -> {tool, info}

function lmToolInfos () {	// contributes.languageModelTools of the running extensions, with a tool registered
  const out = [];
  for (const e of exts)
    for (const t of (e.pkg.contributes && e.pkg.contributes.languageModelTools) || [])
      if (lmTools.has(t.name))
        out.push({name: t.name, description: l10nString(e, t.modelDescription || t.userDescription || t.displayName || ''),
          inputSchema: t.inputSchema, tags: t.tags || []});
  for (const [name] of lmTools) if (!out.some((x) => x.name === name)) out.push({name, description: '', inputSchema: undefined, tags: []});
  return out;
}

const lm = {
  selectChatModels: async () => (lmConfig().ok ? [lmModel()] : []),	// whichever vendor or family is asked for: mme's model
  onDidChangeChatModels: onDidChangeChatModels.event,
  get tools () {
    return lmToolInfos();
  },
  registerTool (name, tool) {
    lmTools.set(name, {tool});
    return new Disposable(() => lmTools.delete(name));
  },
  async invokeTool (name, options, token) {
    const t = lmTools.get(name);
    if (!t) throw new Error('No tool ' + name);
    if (typeof t.tool.prepareInvocation === 'function') await t.tool.prepareInvocation({input: options.input}, token);
    return t.tool.invoke(options, token || new CancellationTokenSource().token);
  },
  registerMcpServerDefinitionProvider: () => new Disposable(() => {}),
};

// chat participants: "Ask @name..." in the palette; the answer streams into an Output channel of its own
const chatParticipants = new Map();	// id -> participant

function chatParticipantList () {	// contributes.chatParticipants: {id, name, fullName, ext}
  const out = [];
  for (const e of exts)
    for (const p of (e.pkg.contributes && e.pkg.contributes.chatParticipants) || [])
      if (p.id) out.push({id: p.id, name: p.name || p.id, fullName: l10nString(e, p.fullName || p.name || p.id), ext: e,
        commands: (p.commands || []).map((c) => c.name)});
  return out;
}

function chatParticipantCommands () {
  return chatParticipantList().map((p) => ({id: '_mme.chat.' + p.id, title: 'Ask @' + p.name + '...', category: p.fullName, ext: p.ext.id}));
}

async function askParticipant (id) {
  const info = chatParticipantList().find((p) => p.id === id);
  if (!chatParticipants.has(id)) await activateOn('onChatParticipant:' + id);
  const part = chatParticipants.get(id);
  if (!part) {
    window.showWarningMessage('@' + (info ? info.name : id) + ' is not there (is its extension running?)');
    return;
  }
  const q = await showInputBox({title: 'Ask @' + (info ? info.name : id), prompt: info && info.commands.length ? '/' + info.commands.join(', /') + ' or a question' : 'A question'});
  if (!q) return;
  let command, prompt = q;
  const m = /^\/(\S+)\s*([\s\S]*)$/.exec(q);
  if (m && info && info.commands.includes(m[1])) {
    command = m[1];
    prompt = m[2];
  }
  const ch = createOutputChannel('Chat: @' + (info ? info.name : id));
  ch.appendLine('> ' + q);	// the channel is there once it has words: then shown
  ch.appendLine('');
  ch.show(true);
  const stream = {
    markdown: (s) => ch.append(typeof s === 'string' ? s : s && s.value ? s.value : ''),
    anchor: (u, title) => ch.append(title || toUri(u).toString()),
    button: (c) => ch.appendLine('[' + (c.title || c.command) + ']'),
    filetree: () => {},
    progress: (s) => ch.appendLine('... ' + s),
    reference: (u) => ch.appendLine('reference: ' + (u && u.uri ? u.uri : u)),
    push: (p) => ch.append(p && p.value ? (p.value.value !== undefined ? p.value.value : String(p.value)) : ''),
    warning: (s) => ch.appendLine('warning: ' + (s && s.value ? s.value : s)),
    confirmation: () => {},
    codeblockUri: () => {},
    textEdit: () => {},
  };
  const request = {prompt, command, references: [], toolReferences: [], toolInvocationToken: undefined, model: lmModel(), id: crypto.randomUUID()};
  try {
    const r = await part.requestHandler(request, {history: []}, stream, new CancellationTokenSource().token);
    if (r && r.errorDetails) ch.appendLine('\n[error] ' + r.errorDetails.message);
  } catch (e) {
    ch.appendLine('\n[error] ' + (e && e.message ? e.message : e));
  }
  ch.appendLine('');
}

const chatNs = {
  createChatParticipant (id, requestHandler) {
    const p = {id, requestHandler, iconPath: undefined, followupProvider: undefined, onDidReceiveFeedback: stubEvent(),
      dispose () {
        chatParticipants.delete(id);
      }};
    chatParticipants.set(id, p);
    return p;
  },
};


// ----------------------------------------------------------------- notebook kernels: the extensions' in mme's notebooks

// notebooks.createNotebookController: in mme's notebook editor, the kernel picker (the toolbar's kernel name)
// lists it; a cell run with it comes here (mme/nbExec), its executeHandler runs, and what its execution
// puts out goes back as the messages mme's own kernel sends (mme/nbMsg: stream, display, error, done).
const NotebookCellKind = {Markup: 1, Code: 2};
const NotebookControllerAffinity = {Default: 1, Preferred: 2};
const NotebookCellStatusBarAlignment = {Left: 1, Right: 2};
const NotebookEditorRevealType = {Default: 0, InCenter: 1, InCenterIfOutsideViewport: 2, AtTop: 3};

class NotebookCellOutputItem {
  constructor (data, mime) {
    this.data = data;
    this.mime = mime;
  }
  static text (v, mime) {
    return new NotebookCellOutputItem(Buffer.from(String(v)), mime || 'text/plain');
  }
  static json (v, mime) {
    return new NotebookCellOutputItem(Buffer.from(JSON.stringify(v, null, 2)), mime || 'text/x-json');
  }
  static stdout (v) {
    return NotebookCellOutputItem.text(v, 'application/vnd.code.notebook.stdout');
  }
  static stderr (v) {
    return NotebookCellOutputItem.text(v, 'application/vnd.code.notebook.stderr');
  }
  static error (e) {
    return new NotebookCellOutputItem(Buffer.from(JSON.stringify({name: e && e.name || 'Error', message: e && e.message || String(e),
      stack: e && e.stack || ''})), 'application/vnd.code.notebook.error');
  }
}

class NotebookCellOutput {
  constructor (items, id, metadata) {
    this.items = items || [];
    this.id = id || crypto.randomUUID();
    this.metadata = metadata || {};
  }
}

class NotebookRange {
  constructor (start, end) {
    this.start = start;
    this.end = end;
    this.isEmpty = start === end;
  }
}

const nbControllers = new Map();	// id -> controller

function sendControllers () {
  notify('mme/nbControllers', {controllers: [...nbControllers.values()].map((c) => ({id: c.id, label: c.label,
    description: c.description || c.detail || '', type: c.notebookType}))});
}

// an output as mme's kernel messages (nbSend puts the cell's id in)
function outputToMsgs (o) {
  const out = [];
  const data = {};
  for (const it of (o && o.items) || []) {
    const text = Buffer.from(it.data || []).toString('utf8');
    if (it.mime === 'application/vnd.code.notebook.stdout') out.push({type: 'stream', name: 'stdout', text});
    else if (it.mime === 'application/vnd.code.notebook.stderr') out.push({type: 'stream', name: 'stderr', text});
    else if (it.mime === 'application/vnd.code.notebook.error') {
      let e = {};
      try {
        e = JSON.parse(text);
      } catch (err) {
        e = {name: 'Error', message: text};
      }
      out.push({type: 'error', ename: e.name || 'Error', evalue: e.message || '', traceback: String(e.stack || '').split('\n')});
    } else if (/^image\/(png|jpeg|gif)$/.test(it.mime)) data[it.mime] = Buffer.from(it.data || []).toString('base64');
    else data[it.mime] = text;
  }
  if (Object.keys(data).length) out.push({type: 'display', data});
  return out;
}

function nbSend (cell, msg) {
  if (cell && cell._mme) notify('mme/nbMsg', {path: cell._mme.path, msg: Object.assign({id: cell._mme.id}, msg)});
}

function createNotebookCellExecution (ctrl, cell) {
  const cts = new CancellationTokenSource();
  let order;
  const exec = {
    cell, token: cts.token,
    get executionOrder () {
      return order;
    },
    set executionOrder (n) {
      order = n;
    },
    start () {},
    end (success) {
      if (success === false && !cell._mme.errored) cell._mme.errored = true;
      nbSend(cell, {type: 'done', count: order || 0});
      if (cell._mme) nbRunning.delete(cell._mme.path + '\n' + cell._mme.id);
    },
    async clearOutput (c) {
      nbSend(c || cell, {type: 'clear'});
    },
    async replaceOutput (outs, c) {
      nbSend(c || cell, {type: 'clear'});
      for (const o of [].concat(outs || [])) for (const m of outputToMsgs(o)) nbSend(c || cell, m);
    },
    async appendOutput (outs, c) {
      for (const o of [].concat(outs || [])) for (const m of outputToMsgs(o)) nbSend(c || cell, m);
    },
    async replaceOutputItems (items) {
      for (const m of outputToMsgs({items: [].concat(items || [])})) nbSend(cell, m);
    },
    async appendOutputItems (items) {
      for (const m of outputToMsgs({items: [].concat(items || [])})) nbSend(cell, m);
    },
  };
  if (cell._mme) nbRunning.set(cell._mme.path + '\n' + cell._mme.id, {exec, cts, ctrl});
  return exec;
}
const nbRunning = new Map();	// "path\nid" -> {exec, cts, ctrl}

function createNotebookController (id, notebookType, label, handler) {
  const onSel = new EventEmitter();
  const c = {
    id, notebookType, label, description: undefined, detail: undefined, supportedLanguages: undefined,
    supportsExecutionOrder: true, executeHandler: handler, interruptHandler: undefined,
    onDidChangeSelectedNotebooks: onSel.event, _onSel: onSel,
    createNotebookCellExecution: (cell) => createNotebookCellExecution(c, cell),
    updateNotebookAffinity () {},
    dispose () {
      nbControllers.delete(id);
      sendControllers();
    },
  };
  nbControllers.set(id, c);
  setTimeout(sendControllers, 0);	// its label, description set just after it is made
  return c;
}

// the notebook and the one cell mme runs, as an extension sees them
function nbObjects (p) {
  const uri = Uri.file(p.path || 'untitled.ipynb');
  const cellUri = uri.with({scheme: 'vscode-notebook-cell', fragment: 'C' + p.id});
  const notebook = {uri, notebookType: p.type || 'jupyter-notebook', version: 1, isDirty: false, isUntitled: !p.path, isClosed: false,
    metadata: {}, get cellCount () {
      return 1;
    }, cellAt: () => cell, getCells: () => [cell], save: async () => true};
  const cell = {index: p.index || 0, kind: 2, notebook, metadata: {}, outputs: [], executionSummary: undefined,
    document: new TextDocument(cellUri, p.language || 'python', 1, p.code || ''), _mme: {path: p.path, id: p.id}};
  return {notebook, cell};
}

async function nbExec (p) {	// mme/nbExec {controller, path, id, code, language}
  const c = nbControllers.get(p.controller);
  const {notebook, cell} = nbObjects(p);
  if (!c) {
    nbSend(cell, {type: 'error', ename: 'Kernel', evalue: 'The kernel ' + p.controller + ' is not there (is its extension running?)', traceback: []});
    nbSend(cell, {type: 'done', count: 0});
    return;
  }
  if (!c._selected) {
    c._selected = true;
    c._onSel.fire({notebook, selected: true});
  }
  try {
    await c.executeHandler.call(c, [cell], notebook, c);
  } catch (e) {
    nbSend(cell, {type: 'error', ename: e && e.name || 'Error', evalue: e && e.message || String(e), traceback: []});
    nbSend(cell, {type: 'done', count: 0});
  }
}

async function nbInterrupt (p) {	// mme/nbInterrupt {controller, path}
  const c = nbControllers.get(p.controller);
  for (const [k, r] of nbRunning)
    if (k.startsWith(p.path + '\n')) r.cts.cancel();
  if (c && typeof c.interruptHandler === 'function') {
    try {
      await c.interruptHandler(nbObjects({path: p.path, id: 0}).notebook);
    } catch (e) {
      log('[error] interrupt: ' + (e && e.message ? e.message : e));
    }
  }
}

const notebooksNs = {
  createNotebookController,
  registerNotebookSerializer: () => new Disposable(() => {}),	// mme reads and writes .ipynb itself
  registerNotebookCellStatusBarItemProvider: () => new Disposable(() => {}),
  createRendererMessaging: () => ({onDidReceiveMessage: stubEvent(), postMessage: async () => false}),
  onDidOpenNotebookDocument: stubEvent(), onDidCloseNotebookDocument: stubEvent(),
};


// ----------------------------------------------------------------- webviews: in the browser

// mme draws cells, not HTML: an extension's webview (a chat panel, a preview) is served here on
// 127.0.0.1 and opened in the browser. acquireVsCodeApi() is given to the page; its postMessage
// comes back by POST, the extension's by server-sent events. A random token in every path keeps
// other programs of the machine out.
const http = require('http');
const wvToken = crypto.randomBytes(16).toString('hex');
let wvServer = null, wvPort = 0, wvStarting = null;
const webviews = new Map();	// id -> Webview
let nextWebview = 1;

function wvStart () {
  if (wvServer) return Promise.resolve(wvPort);
  if (wvStarting) return wvStarting;
  wvStarting = new Promise((resolve, reject) => {
    const srv = http.createServer((req, res) => wvServe(req, res).catch((e) => {
      log('[error] webview server: ' + (e && e.stack ? e.stack : e));
      try {
        res.writeHead(500);
        res.end();
      } catch (err) {
        // the answer was begun
      }
    }));
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      wvServer = srv;
      wvPort = srv.address().port;
      srv.unref();	// it does not keep the host alive
      resolve(wvPort);
    });
  });
  return wvStarting;
}

function wvOrigin () {
  return 'http://127.0.0.1:' + wvPort;
}

const wvMime = {'.html': 'text/html; charset=utf-8', '.htm': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8', '.cjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.map': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp', '.ico': 'image/x-icon', '.woff': 'font/woff',
  '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.otf': 'font/otf', '.wasm': 'application/wasm', '.txt': 'text/plain; charset=utf-8',
  '.md': 'text/plain; charset=utf-8', '.mp4': 'video/mp4', '.webm': 'video/webm', '.mp3': 'audio/mpeg', '.wav': 'audio/wav'};

// a file as the page asks for it: /<token>/res/<drive>/<path> on Windows, /<token>/res/<path> elsewhere
function wvResourceUri (u) {
  const f = toUri(u).fsPath;
  let p = isWin ? f.replace(/\\/g, '/') : f;
  if (isWin && /^[a-zA-Z]:/.test(p)) p = '/' + p[0].toLowerCase() + p.slice(2);
  return new Uri('http', '127.0.0.1:' + wvPort, '/' + wvToken + '/res' + p, '', '');
}

function wvResourcePath (rest) {
  let p = rest.split('/').map((s) => decodeURIComponent(s)).join('/');
  if (isWin && /^\/[a-zA-Z](\/|$)/.test(p)) p = p[1] + ':' + (p.slice(2) || '/');
  return path.normalize(p);
}

// the folders a webview may load from: its localResourceRoots, else its extension's and the workspace's
function wvAllowed (file) {
  const roots = [];
  for (const w of webviews.values()) {
    const o = w._options || {};
    if (Array.isArray(o.localResourceRoots)) for (const r of o.localResourceRoots) roots.push(toUri(r).fsPath);
    if (w._ext) roots.push(w._ext.dir);
  }
  for (const e of exts) roots.push(e.dir);
  for (const f of folders) roots.push(f.uri.fsPath);
  if (dataDir) roots.push(path.join(dataDir, 'extension-state'));
  const low = (s) => (isWin ? path.normalize(s).toLowerCase() : path.normalize(s));
  const f = low(file);
  return roots.some((r) => {
    const x = low(r);
    return f === x || f.startsWith(x.endsWith(path.sep) ? x : x + path.sep);
  });
}

function readBody (req) {
  return new Promise((resolve, reject) => {
    const parts = [];
    let n = 0;
    req.on('data', (d) => {
      n += d.length;
      if (n > 64 * 1024 * 1024) req.destroy();
      else parts.push(d);
    });
    req.on('end', () => resolve(Buffer.concat(parts).toString('utf8')));
    req.on('error', reject);
  });
}

async function wvServe (req, res) {
  const url = new URL(req.url, 'http://127.0.0.1');
  const parts = url.pathname.split('/');	// '', token, kind, ...
  if (parts[1] !== wvToken) {
    res.writeHead(404);
    res.end();
    return;
  }
  const noCache = {'Cache-Control': 'no-store'};
  if (parts[2] === 'res') {
    const file = wvResourcePath('/' + parts.slice(3).join('/'));
    if (!wvAllowed(file)) {
      res.writeHead(403, noCache);
      res.end();
      return;
    }
    let data;
    try {
      data = await fs.promises.readFile(file);
    } catch (e) {
      res.writeHead(404, noCache);
      res.end();
      return;
    }
    res.writeHead(200, {'Content-Type': wvMime[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Access-Control-Allow-Origin': '*', ...noCache});
    res.end(data);
    return;
  }
  if (parts[2] !== 'wv') {
    res.writeHead(404);
    res.end();
    return;
  }
  const w = webviews.get(parts[3]);
  const what = parts[4] || '';
  if (!w) {
    res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', ...noCache});
    res.end('<!doctype html><title>Closed</title><body style="font-family:sans-serif;padding:2em">This panel was closed in mme.</body>');
    return;
  }
  if (what === '' && req.method === 'GET') {
    const page = await w._page();
    res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', ...noCache});
    res.end(page);
  }
  else if (what === 'events') {
    res.writeHead(200, {'Content-Type': 'text/event-stream', 'Connection': 'keep-alive', ...noCache});
    res.write(': mme\n\n');
    w._connect(res);
  }
  else if (what === 'post' && req.method === 'POST') {
    const body = await readBody(req);
    res.writeHead(204, noCache);
    res.end();
    let m;
    try {
      m = JSON.parse(body);
    } catch (e) {
      return;
    }
    w._onMessage.fire(m);
  }
  else if (what === 'state' && req.method === 'POST') {
    const body = await readBody(req);
    res.writeHead(204, noCache);
    res.end();
    try {
      w._state = JSON.parse(body);
    } catch (e) {
      // not JSON: kept as it was
    }
  }
  else {
    res.writeHead(404, noCache);
    res.end();
  }
}

// VS Code's theme as CSS variables: mme's colors (mme/theme), and what VS Code derives from them
async function wvTheme () {
  let t = null;
  try {
    t = await request('mme/theme', {});
  } catch (e) {
    t = null;
  }
  const c = Object.assign({}, (t && t.colors) || {});
  const light = !!(t && t.kind === 1);
  const d = (k, v) => {
    if (!c[k] && v) c[k] = v;
  };
  const bg = c['editor.background'] || (light ? '#ffffff' : '#1f1f1f');
  const fg = c['editor.foreground'] || (light ? '#3b3b3b' : '#cccccc');
  d('editor.background', bg);
  d('editor.foreground', fg);
  d('foreground', c['sideBar.foreground'] || fg);
  d('sideBar.background', bg);
  d('panel.background', bg);
  d('editorWidget.background', c['menu.background'] || bg);
  d('editorWidget.foreground', c['menu.foreground'] || fg);
  d('editorWidget.border', c['sideBar.border']);
  d('widget.border', c['sideBar.border']);
  d('panel.border', c['sideBar.border']);
  d('button.background', c['focusBorder'] || '#0078d4');
  d('button.foreground', '#ffffff');
  d('button.hoverBackground', c['focusBorder'] || '#026ec1');
  d('button.secondaryBackground', c['input.background']);
  d('button.secondaryForeground', c['input.foreground'] || fg);
  d('button.secondaryHoverBackground', c['list.hoverBackground']);
  d('button.border', 'transparent');
  d('badge.background', c['focusBorder']);
  d('badge.foreground', '#ffffff');
  d('input.border', c['sideBar.border']);
  d('dropdown.background', c['input.background']);
  d('dropdown.foreground', c['input.foreground']);
  d('dropdown.border', c['sideBar.border']);
  d('dropdown.listBackground', c['menu.background']);
  d('checkbox.background', c['input.background']);
  d('checkbox.foreground', c['input.foreground']);
  d('checkbox.border', c['sideBar.border']);
  d('textLink.foreground', light ? '#005fb8' : '#4daafc');
  d('textLink.activeForeground', light ? '#005fb8' : '#4daafc');
  d('textPreformat.foreground', light ? '#3b3b3b' : '#d0d0d0');
  d('textBlockQuote.background', c['sideBar.background']);
  d('textCodeBlock.background', c['input.background']);
  d('textSeparator.foreground', c['sideBar.border']);
  d('errorForeground', c['editorError.foreground']);
  d('icon.foreground', fg);
  d('toolbar.hoverBackground', c['list.hoverBackground']);
  d('list.focusBackground', c['list.activeSelectionBackground']);
  d('list.focusForeground', c['list.activeSelectionForeground']);
  d('quickInput.foreground', c['menu.foreground']);
  d('scrollbarSlider.activeBackground', c['scrollbarSlider.hoverBackground']);
  d('progressBar.background', c['focusBorder']);
  d('editorHoverWidget.background', c['menu.background']);
  d('editorHoverWidget.foreground', c['menu.foreground']);
  d('editorHoverWidget.border', c['sideBar.border']);
  d('menu.border', c['sideBar.border']);
  d('notifications.foreground', c['menu.foreground']);
  d('settings.headerForeground', fg);
  d('chat.requestBackground', c['input.background']);
  d('chat.slashCommandBackground', c['list.hoverBackground']);
  d('keybindingLabel.background', c['input.background']);
  d('keybindingLabel.foreground', fg);
  d('keybindingLabel.border', c['sideBar.border']);
  const font = isWin ? '"Segoe WPC", "Segoe UI", sans-serif' : process.platform === 'darwin'
    ? '-apple-system, BlinkMacSystemFont, sans-serif' : 'system-ui, "Ubuntu", "Droid Sans", sans-serif';
  const edFont = (settingValue('editor.fontFamily') || '') || (isWin ? 'Consolas, "Courier New", monospace' : 'Menlo, Monaco, "Courier New", monospace');
  const edSize = settingValue('editor.fontSize') || 14;
  const vars = [`--vscode-font-family: ${font}`, '--vscode-font-weight: normal', '--vscode-font-size: 13px',
    `--vscode-editor-font-family: ${edFont}`, '--vscode-editor-font-weight: normal', `--vscode-editor-font-size: ${edSize}px`];
  for (const [k, v] of Object.entries(c)) if (typeof v === 'string') vars.push('--vscode-' + k.replace(/\./g, '-') + ': ' + v);
  return {light, css: ':root {\n  ' + vars.join(';\n  ') + ';\n}'};
}

// VS Code's default styles for a webview, and the bridge its scripts talk through
function wvHead (w, theme) {
  const boot = `(function () {
  var base = location.pathname.replace(/[^/]*$/, '');
  var state = ${JSON.stringify(w._state === undefined ? null : w._state).replace(/</g, '\\u003c')};
  var chain = Promise.resolve();
  function send (what, v) {
    var body = JSON.stringify(v === undefined ? null : v);
    chain = chain.then(function () {
      return fetch(base + what, {method: 'POST', body: body, headers: {'Content-Type': 'application/json'}}).catch(function () {});
    });
  }
  var api = Object.freeze({
    postMessage: function (m) { send('post', m); },
    getState: function () { return state === null ? undefined : state; },
    setState: function (s) { state = s; send('state', s); return s; }
  });
  window.acquireVsCodeApi = function () { return api; };
  function listen () {
    var es = new EventSource(base + 'events');
    es.onmessage = function (e) {
      var d = JSON.parse(e.data);
      if (d.type === 'message') window.dispatchEvent(new MessageEvent('message', {data: d.data, origin: location.origin}));
      else if (d.type === 'reload') location.reload();
      else if (d.type === 'title') document.title = d.title;
      else if (d.type === 'close') {
        es.close();
        window.close();
        document.body.innerHTML = '<p style="padding:2em">This panel was closed in mme.</p>';
      }
    };
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', function () { setTimeout(listen, 0); });
  else setTimeout(listen, 0);
})();`;
  const style = `${theme.css}
html { scrollbar-color: var(--vscode-scrollbarSlider-background) transparent; }
body { background-color: var(--vscode-${w._view ? 'sideBar' : 'editor'}-background); color: var(--vscode-foreground);
  font-family: var(--vscode-font-family); font-weight: var(--vscode-font-weight); font-size: var(--vscode-font-size);
  margin: 0; padding: 0 20px; }
img, video { max-width: 100%; max-height: 100%; }
a, a code { color: var(--vscode-textLink-foreground); }
a:hover { color: var(--vscode-textLink-activeForeground); }
code { font-family: var(--vscode-editor-font-family); color: var(--vscode-textPreformat-foreground); }
button:focus, input:focus, select:focus, textarea:focus { outline: 1px solid var(--vscode-focusBorder); outline-offset: -1px; }
input, textarea, select { background: var(--vscode-input-background); color: var(--vscode-input-foreground); }`;
  return `<meta charset="utf-8"><title>${htmlEscape(w._title || 'mme')}</title><style id="_defaultStyles">${style}</style><script>${boot}</script>`;
}

function htmlEscape (s) {
  return String(s).replace(/[&<>"]/g, (ch) => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;'}[ch]));
}

class Webview {
  constructor (ext, options, title, view) {
    this._id = String(nextWebview++);
    this._ext = ext;
    this._options = Object.assign({}, options || {});
    this._html = '';
    this._title = title || '';
    this._view = !!view;
    this._state = undefined;
    this._clients = new Set();
    this._queue = [];
    this._onMessage = new EventEmitter();
    this._onVisible = new EventEmitter();	// clients came or all went: the panel's / view's visibility
    this.onDidReceiveMessage = this._onMessage.event;
    webviews.set(this._id, this);
  }
  get html () {
    return this._html;
  }
  set html (s) {
    this._html = String(s === undefined || s === null ? '' : s);
    this._send({type: 'reload'}, false);
  }
  get options () {
    return this._options;
  }
  set options (o) {
    this._options = Object.assign({}, o || {});
  }
  get cspSource () {
    return wvOrigin();
  }
  asWebviewUri (u) {
    return wvResourceUri(u);
  }
  postMessage (m) {
    const data = plain(m);
    if (this._clients.size === 0) {	// not opened yet (or closed): it waits, as VS Code keeps them for a page loading
      this._queue.push(data);
      if (this._queue.length > 1000) this._queue.shift();
      return Promise.resolve(false);
    }
    this._send({type: 'message', data}, true);
    return Promise.resolve(true);
  }
  _send (ev, queue) {
    const line = 'data: ' + JSON.stringify(ev) + '\n\n';
    for (const res of this._clients) res.write(line);
    if (queue && this._clients.size === 0) this._queue.push(ev.data);
  }
  _connect (res) {
    const first = this._clients.size === 0;
    this._clients.add(res);
    const ping = setInterval(() => res.write(': ping\n\n'), 20000);
    res.on('close', () => {
      clearInterval(ping);
      this._clients.delete(res);
      if (this._clients.size === 0) setTimeout(() => {
        if (this._clients.size === 0) this._onVisible.fire(false);
      }, 3000);
    });
    const q = this._queue;
    this._queue = [];
    for (const data of q) res.write('data: ' + JSON.stringify({type: 'message', data}) + '\n\n');
    if (first) this._onVisible.fire(true);
  }
  async _page () {
    const theme = await wvTheme();
    let html = this._html || '<!doctype html><html><head></head><body><p style="opacity:.7">Loading...</p></body></html>';
    // the page is ours to serve: its Content-Security-Policy is for VS Code's iframe (nonces for scripts
    // that our bridge would not have); the token in the path already keeps it to this machine's browser
    html = html.replace(/<meta\s+[^>]*http-equiv\s*=\s*["']?content-security-policy["']?[^>]*>/gi, '');
    // the old vscode-resource: links
    html = html.replace(/vscode-resource:(\/\/[^"')\s]*)?(\/[^"')\s]*)/g, (m, auth, p) => wvResourceUri(Uri.file(p)).toString());
    const head = wvHead(this, theme);
    const cls = theme.light ? 'vscode-light' : 'vscode-dark';
    if (/<head[^>]*>/i.test(html)) html = html.replace(/<head[^>]*>/i, (m) => m + head);
    else if (/<html[^>]*>/i.test(html)) html = html.replace(/<html[^>]*>/i, (m) => m + '<head>' + head + '</head>');
    else html = '<!doctype html><html><head>' + head + '</head><body>' + html + '</body></html>';
    if (/<body[^>]*>/i.test(html))
      html = html.replace(/<body([^>]*)>/i, (m, a) => {
        if (/class\s*=\s*["']/i.test(a)) return '<body' + a.replace(/class\s*=\s*(["'])/i, (x, q) => 'class=' + q + cls + ' ') + ` data-vscode-theme-kind="${cls}">`;
        return `<body${a} class="${cls}" data-vscode-theme-kind="${cls}">`;
      });
    return html;
  }
  _dispose () {
    this._send({type: 'close'}, false);
    for (const res of this._clients) res.end();
    this._clients.clear();
    webviews.delete(this._id);
  }
  async _open () {
    await wvStart();
    const url = wvOrigin() + '/' + wvToken + '/wv/' + this._id + '/';
    log('[info] webview "' + this._title + '" opened in the browser: ' + url.replace(wvToken, '<token>'));
    await request('window/showDocument', {uri: url, external: true}).catch(() => {});
    window.setStatusBarMessage('$(globe) ' + (this._title || 'A webview') + ': opened in the browser', 6000);
  }
}

// the extension whose code is running now (its webview's resources are its folder's)
function callerExtension () {
  const st = new Error().stack || '';
  const low = isWin ? st.toLowerCase() : st;
  return exts.find((e) => low.includes(isWin ? e.dir.toLowerCase() : e.dir));
}

function createWebviewPanel (viewType, title, showOptions, options, forExt) {
  const ext = forExt || callerExtension();
  const w = new Webview(ext, options, title, false);
  const onDispose = new EventEmitter(), onState = new EventEmitter();
  let disposed = false, visible = false;
  const panel = {
    webview: w, viewType, options: Object.assign({}, options || {}),
    get title () {
      return w._title;
    },
    set title (t) {
      w._title = String(t);
      w._send({type: 'title', title: w._title}, false);
    },
    iconPath: undefined,
    get viewColumn () {
      return typeof showOptions === 'number' ? showOptions : (showOptions && showOptions.viewColumn) || 1;
    },
    get active () {
      return visible;
    },
    get visible () {
      return visible;
    },
    onDidDispose: onDispose.event,
    onDidChangeViewState: onState.event,
    reveal () {
      if (!disposed && w._clients.size === 0) w._open();
    },
    dispose () {
      if (disposed) return;
      disposed = true;
      w._dispose();
      onDispose.fire();
      onDispose.dispose();
    },
  };
  w._onVisible.event((v) => {
    if (visible === v) return;
    visible = v;
    onState.fire({webviewPanel: panel});
  });
  const preserve = showOptions && typeof showOptions === 'object' && showOptions.preserveFocus;
  setTimeout(() => {	// its html is set right after it is made: the page is asked for then
    if (!disposed) w._open();
  }, preserve ? 300 : 50);
  return panel;
}

// webview views: an extension's side bar panels (its chat): opened by "<view>.focus" or its container's command
const viewProviders = new Map();	// view id -> {provider, options}
const openViews = new Map();	// view id -> {view, webview}

function webviewViews () {	// every contributed webview view: {id, name, container, ext}
  const out = [];
  for (const e of exts) {
    const c = e.pkg.contributes || {};
    const containers = {};
    for (const list of Object.values(c.viewsContainers || {}))
      for (const vc of list || []) containers[vc.id] = l10nString(e, vc.title);
    for (const [container, list] of Object.entries(c.views || {}))
      for (const v of list || [])
        if (v.type === 'webview') out.push({id: v.id, name: l10nString(e, v.name) || v.id, container,
          containerTitle: containers[container] || l10nString(e, e.pkg.displayName) || e.id, ext: e});
  }
  return out;
}

function registerWebviewViewProvider (viewId, provider, options) {
  viewProviders.set(viewId, {provider, options: options || {}});
  return new Disposable(() => viewProviders.delete(viewId));
}

async function openView (viewId) {
  const known = openViews.get(viewId);
  if (known) {
    if (known.webview._clients.size === 0) await known.webview._open();
    return true;
  }
  if (!viewProviders.has(viewId)) await activateOn('onView:' + viewId);
  const p = viewProviders.get(viewId);
  const info = webviewViews().find((v) => v.id === viewId);
  if (!p) {
    window.showWarningMessage((info ? info.name : viewId) + ': the extension gives no view for it yet (is it running and signed in?)');
    return false;
  }
  const title = info ? (info.containerTitle && info.containerTitle !== info.name ? info.containerTitle + ': ' + info.name : info.name) : viewId;
  const w = new Webview(info ? info.ext : callerExtension(), (p.options && p.options.webviewOptions) || {}, title, true);
  const onDispose = new EventEmitter(), onVis = new EventEmitter();
  let visible = false;
  const view = {
    webview: w, viewType: viewId,
    get title () {
      return w._title;
    },
    set title (t) {
      if (t) {
        w._title = String(t);
        w._send({type: 'title', title: w._title}, false);
      }
    },
    description: undefined, badge: undefined,
    get visible () {
      return visible;
    },
    onDidDispose: onDispose.event,
    onDidChangeVisibility: onVis.event,
    show () {
      if (w._clients.size === 0) w._open();
    },
  };
  w._onVisible.event((v) => {
    if (visible === v) return;
    visible = v;
    onVis.fire();
  });
  openViews.set(viewId, {view, webview: w});
  const cts = new CancellationTokenSource();
  try {
    await p.provider.resolveWebviewView(view, {state: undefined}, cts.token);
  } catch (e) {
    log('[error] ' + viewId + ': resolveWebviewView: ' + (e && e.stack ? e.stack : e));
  }
  await w._open();
  return true;
}

// the palette's commands for the webview views: VS Code's "<view>.focus", and "workbench.view.extension.<container>"
function webviewViewCommands () {
  const out = [];
  for (const v of webviewViews())
    out.push({id: v.id + '.focus', title: 'Open ' + v.name + ' (in the browser)', category: v.containerTitle, ext: v.ext.id});
  return out;
}

async function viewCommand (id) {	// a command that opens a webview view; undefined: not one
  const views = webviewViews();
  if (id.endsWith('.focus')) {
    const v = views.find((x) => x.id === id.slice(0, -6));
    if (v) return openView(v.id);
  }
  if (id.startsWith('workbench.view.extension.')) {
    const v = views.find((x) => x.container === id.slice(25));
    if (v) return openView(v.id);
  }
  return undefined;
}


// custom editors (an extension's own editor for a file type: a diagram, a hex view): the file in front
// reopened with one, in the browser - "Reopen Active File With <editor>" in the palette
const customEditors = new Map();	// viewType -> {provider, options}

function registerCustomEditorProvider (viewType, provider, options) {
  customEditors.set(viewType, {provider, options: options || {}});
  return new Disposable(() => customEditors.delete(viewType));
}

function customEditorList () {	// every contributed one: {viewType, name, patterns, ext}
  const out = [];
  for (const e of exts)
    for (const c of (e.pkg.contributes && e.pkg.contributes.customEditors) || [])
      out.push({viewType: c.viewType, name: l10nString(e, c.displayName) || c.viewType, ext: e,
        patterns: (c.selector || []).map((x) => x.filenamePattern).filter(Boolean)});
  return out;
}

function editorPatternMatch (pattern, file) {	// "*.drawio", "**/*.{png,jpg}": against the file's name, or its path for a "/"
  const name = pattern.includes('/') ? file.replace(/\\/g, '/') : path.basename(file);
  let re = '';
  for (let i = 0; i < pattern.length; i++) {
    const ch = pattern[i];
    if (ch === '*' && pattern[i + 1] === '*') {
      re += '.*';
      i++;
      if (pattern[i + 1] === '/') i++;
    } else if (ch === '*') re += '[^/]*';
    else if (ch === '?') re += '[^/]';
    else if (ch === '{') re += '(';
    else if (ch === '}') re += ')';
    else if (ch === ',') re += '|';
    else re += ch.replace(/[.+^$()|[\]\\]/g, '\\$&');
  }
  return new RegExp((pattern.includes('/') ? '(^|/)' : '^') + re + '$', 'i').test(name);
}

async function openCustomEditor (viewType, uri) {
  const info = customEditorList().find((c) => c.viewType === viewType);
  if (!customEditors.has(viewType)) await activateOn('onCustomEditor:' + viewType);
  const c = customEditors.get(viewType);
  if (!c) {
    window.showWarningMessage((info ? info.name : viewType) + ': the extension gives no editor for it (is it running?)');
    return false;
  }
  const title = path.basename(uri.fsPath) + ' - ' + (info ? info.name : viewType);
  const panel = createWebviewPanel(viewType, title, 1, (c.options && c.options.webviewOptions) || {enableScripts: true}, info && info.ext);
  const cts = new CancellationTokenSource();
  try {
    if (typeof c.provider.resolveCustomTextEditor === 'function') {	// a text file: its edits go to mme's document
      const doc = docFor(uri) || await openTextDocument(uri);
      await c.provider.resolveCustomTextEditor(doc, panel, cts.token);
    } else {
      const cd = await c.provider.openCustomDocument(uri, {backupId: undefined, untitledDocumentData: undefined}, cts.token);
      await c.provider.resolveCustomEditor(cd, panel, cts.token);
    }
  } catch (e) {
    log('[error] ' + viewType + ': ' + (e && e.stack ? e.stack : e));
  }
  return true;
}

function customEditorCommands () {
  return customEditorList().map((c) => ({id: '_mme.customEditor.' + c.viewType,
    title: 'Reopen Active File With ' + c.name + ' (in the browser)', category: l10nString(c.ext, c.ext.pkg.displayName) || c.ext.id, ext: c.ext.id}));
}

async function customEditorCommand (id) {	// "_mme.customEditor.<viewType>": the file in front; undefined: not one
  if (!id.startsWith('_mme.customEditor.')) return undefined;
  const viewType = id.slice(18);
  const info = customEditorList().find((c) => c.viewType === viewType);
  if (!activeEditor) {
    window.showWarningMessage('Open a file first, then reopen it with ' + (info ? info.name : viewType));
    return false;
  }
  const f = activeEditor.document.uri.fsPath;
  if (info && info.patterns.length && !info.patterns.some((pt) => editorPatternMatch(pt, f)))
    window.showInformationMessage(info.name + ' is for ' + info.patterns.join(', ') + ': trying it on ' + path.basename(f) + ' anyway');
  return openCustomEditor(viewType, activeEditor.document.uri);
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
  createWebviewPanel: (a, b, c, d) => createWebviewPanel(a, b, c, d), registerWebviewViewProvider, registerCustomEditorProvider,
  createTreeView, registerTreeDataProvider,
  registerWebviewPanelSerializer: () => new Disposable(() => {}),	// a panel is not brought back after a restart
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
  registerDocumentSemanticTokensProvider: reg('semanticTokens'),
  registerDocumentRangeSemanticTokensProvider: reg('semanticTokensRange'),
  registerCallHierarchyProvider: reg('callHierarchy'),
  registerTypeHierarchyProvider: reg('typeHierarchy'),
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
    taskProviders.set(type, p);
    return new Disposable(() => taskProviders.delete(type));
  },
  fetchTasks: async (filter) => collectTasks(filter && filter.type),
  executeTask,
  get taskExecutions () {
    return taskExecutions;
  },
  onDidStartTask: onDidStartTask.event, onDidEndTask: onDidEndTask.event,
  onDidStartTaskProcess: onDidStartTaskProcess.event, onDidEndTaskProcess: onDidEndTaskProcess.event,
}, 'tasks');

const debug = spare({
  get activeDebugSession () {
    return activeDebugSession;
  },
  activeDebugConsole: debugConsole, breakpoints: [], activeStackItem: undefined,
  onDidStartDebugSession: onDidStartDebugSession.event, onDidTerminateDebugSession: onDidTerminateDebugSession.event,
  onDidChangeActiveDebugSession: onDidChangeActiveDebugSession.event,
  onDidReceiveDebugSessionCustomEvent: onDidReceiveDebugSessionCustomEvent.event,
  onDidChangeBreakpoints: onDidChangeBreakpoints.event, onDidChangeActiveStackItem: stubEvent(),
  registerDebugConfigurationProvider (type, provider, trigger) {
    const x = {type, provider, trigger: trigger || 1};
    debugConfigProviders.push(x);
    return new Disposable(() => {
      const i = debugConfigProviders.indexOf(x);
      if (i >= 0) debugConfigProviders.splice(i, 1);
    });
  },
  registerDebugAdapterDescriptorFactory (type, factory) {
    debugAdapterFactories.set(type, factory);
    return new Disposable(() => debugAdapterFactories.delete(type));
  },
  registerDebugAdapterTrackerFactory (type, factory) {	// mme sends it the session's messages (mme/dapIn, mme/dapOut)
    const x = {type, factory};
    debugTrackerFactories.push(x);
    return new Disposable(() => {
      const i = debugTrackerFactories.indexOf(x);
      if (i >= 0) debugTrackerFactories.splice(i, 1);
    });
  },
  startDebugging,
  stopDebugging: async () => notify('mme/stopDebugging', {}),
  addBreakpoints () {},
  removeBreakpoints () {},
  asDebugSourceUri: (src) => Uri.file(src.path || ''),
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
  RelativePattern, TreeItem, FileSystemError, TestRunRequest, TestMessage, TestTag, TaskGroup, ShellExecution, ProcessExecution, CustomExecution, Task, DebugAdapterExecutable,
  DebugAdapterServer, DebugAdapterNamedPipeServer, DebugAdapterInlineImplementation, LanguageModelChatMessage,
  LanguageModelTextPart, LanguageModelToolCallPart, LanguageModelToolResultPart, LanguageModelToolResult, LanguageModelPromptTsxPart,
  LanguageModelDataPart, LanguageModelChatMessageRole, LanguageModelChatToolMode, NotebookCellOutputItem, NotebookCellOutput,
  NotebookRange, NotebookCellKind, NotebookControllerAffinity, NotebookCellStatusBarAlignment, NotebookEditorRevealType, NotebookCellData, NotebookData, LanguageModelError, TextDocument, TextLine,
  ...enums, ...moreClasses,
  window, workspace, languages, commands: commandsNs, env, extensions: extensionsNs, tasks, debug, l10n,
  scm: spare({createSourceControl: undefined, inputBox: undefined}, 'scm'),
  comments: spare({}, 'comments'), authentication: spare(authentication, 'authentication'),
  notebooks: spare(notebooksNs, 'notebooks'), tests: spare({createTestController}, 'tests'), chat: spare(chatNs, 'chat'), lm: spare(lm, 'lm'),
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
    const missing = missingDeps(e);
    if (missing.length) {	// Flutter without Dart: it cannot work, and it is not started
      e.failed = true;
      log('[info] ' + e.id + ' is not started: it needs ' + missing.join(', '));
      notify('mme/extensionState', {id: e.id, state: 'needs ' + missing.map((m) => m.split('.').pop()).join(', ')});
      return;
    }
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

// the extensions it depends on that do not run here (VS Code's own vscode.* are there always)
function missingDeps (e) {
  return (e.pkg.extensionDependencies || []).filter((dep) => !/^vscode\./i.test(dep) && !exts.some((x) => x.id === dep.toLowerCase()));
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
  for (const list of Object.values(c.views || {})) for (const v of list || []) if (v.id) events.push('onView:' + v.id);
  for (const ce of c.customEditors || []) if (ce.viewType) events.push('onCustomEditor:' + ce.viewType);
  for (const td of c.taskDefinitions || []) if (td.type) events.push('onTaskType:' + td.type);
  for (const cp of c.chatParticipants || []) if (cp.id) events.push('onChatParticipant:' + cp.id);
  for (const t of c.languageModelTools || []) if (t.name) events.push('onLanguageModelTool:' + t.name);
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
  await wvStart().catch((e) => log('[error] the webview server: ' + e.message));	// its port is in asWebviewUri's links, asked for at once
  // the palette's commands: what the running extensions contribute
  const cmds = [];
  for (const e of exts) for (const c of (e.pkg.contributes && e.pkg.contributes.commands) || []) {
    const t = typeof c.title === 'object' ? c.title.value : c.title;
    const cat = typeof c.category === 'object' ? c.category.value : c.category;
    cmds.push({id: c.command, title: l10nString(e, t), category: l10nString(e, cat) || '', ext: e.id});
  }
  cmds.push(...webviewViewCommands(), ...customEditorCommands(), ...chatParticipantCommands());	// "Open <view> (in the browser)", "Reopen Active File With ..."
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
  for (const e of exts) {	// one that cannot start says why at once, not "starting" for ever
    const missing = missingDeps(e);
    if (missing.length) notify('mme/extensionState', {id: e.id, state: 'needs ' + missing.map((m) => m.split('.').pop()).join(', ')});
  }
  notify('mme/contributions', {commands: cmds, keybindings: keys, configuration: props});
  notify('mme/treeViews', {views: treeViewList().map((v) => ({id: v.id, name: v.name, title: v.title}))});
  notify('mme/debuggers', {types: debuggerTypes().map((d) => ({type: d.type, label: d.label}))});
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
    case 'exit': quit(); break;
    case '$/cancelRequest': {
      const s = cancels.get(params.id);
      if (s) s.cancel();
      break;
    }
    case 'textDocument/didOpen': didOpen(params); break;
    case 'textDocument/didChange': didChange(params); break;
    case 'textDocument/didSave': didSave(params); break;
    case 'mme/treeExpand': treeExpand(params || {}); break;	// the side bar's extension views
    case 'mme/provideTasks': provideTasksToMme(); break;	// Run Task: the extensions' tasks
    case 'mme/debugResolve': resolveDebug(params || {}); break;	// a launch configuration of an extension's type
    case 'mme/debugEnded': debugEnded(); break;
    case 'mme/nbExec': nbExec(params || {}); break;	// a notebook cell, run with an extension's kernel
    case 'mme/nbInterrupt': nbInterrupt(params || {}); break;
    case 'mme/dapIn': dapIn(params || {}); break;
    case 'mme/dapOut': dapOut(params || {}); break;
    case 'mme/debugResponse': debugResponse(params || {}); break;
    case 'mme/testDiscover': discoverTests(); break;	// the Testing view: the extensions' tests
    case 'mme/testRun': runTestsFromMme(params || {}); break;
    case 'mme/testCancel': cancelTestRuns(); break;
    case 'mme/runTask': runTaskFromMme(params || {}); break;
    case 'mme/treeSelect': treeSelect(params || {}); break;
    case 'mme/treeAction': treeAction(params || {}); break;
    case 'window/workDoneProgress/cancel': {	// the progress item clicked: Cancel
      const c = params && progressCancels.get(params.token);
      if (c) c.cancel();
      break;
    }
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
    semanticTokensProvider: {legend: {tokenTypes: SEM_TYPES, tokenModifiers: SEM_MODS}, full: true, range: true},
    callHierarchyProvider: true, typeHierarchyProvider: true,
    executeCommandProvider: {commands: []},
  };
}
