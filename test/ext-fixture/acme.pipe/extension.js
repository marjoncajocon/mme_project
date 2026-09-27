// a debug adapter on a named pipe (a Unix socket elsewhere), the way some debuggers are reached:
// DebugAdapterNamedPipeServer; the adapter is a little server of the extension's own on that pipe
const vscode = require('vscode');
const net = require('net');
const os = require('os');
const path = require('path');

function adapter (sock) {
  let buf = Buffer.alloc(0), seq = 1;
  const send = (m) => {
    const body = Buffer.from(JSON.stringify(Object.assign({seq: seq++}, m)), 'utf8');
    sock.write('Content-Length: ' + body.length + '\r\n\r\n');
    sock.write(body);
  };
  const reply = (req, body) => send({type: 'response', request_seq: req.seq, success: true, command: req.command, body});
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      const sep = buf.indexOf('\r\n\r\n');
      if (sep < 0) return;
      const n = +/Content-Length:\s*(\d+)/i.exec(buf.slice(0, sep).toString())[1];
      if (buf.length < sep + 4 + n) return;
      const m = JSON.parse(buf.slice(sep + 4, sep + 4 + n).toString('utf8'));
      buf = buf.slice(sep + 4 + n);
      if (m.command === 'initialize') {
        reply(m, {supportsConfigurationDoneRequest: true});
        send({type: 'event', event: 'initialized'});
      } else if (m.command === 'configurationDone') {
        reply(m, {});
        send({type: 'event', event: 'output', body: {category: 'stdout', output: 'hello over the named pipe\n'}});
      } else if (m.command === 'threads') reply(m, {threads: []});
      else reply(m, {});
    }
  });
  sock.on('error', () => {});
}

function activate (context) {
  const pipe = process.platform === 'win32' ? '\\\\.\\pipe\\acme-pipe-' + process.pid
    : path.join(os.tmpdir(), 'acme-pipe-' + process.pid + '.sock');
  const server = net.createServer(adapter);
  server.listen(pipe);
  context.subscriptions.push({dispose: () => server.close()});
  context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('acmepipe', {
    createDebugAdapterDescriptor: () => new vscode.DebugAdapterNamedPipeServer(pipe),
  }));
  context.subscriptions.push(vscode.commands.registerCommand('acmePipe.debug', () =>
    vscode.debug.startDebugging(undefined, {type: 'acmepipe', name: 'Acme Pipe', request: 'launch'})));
}

module.exports = {activate};
