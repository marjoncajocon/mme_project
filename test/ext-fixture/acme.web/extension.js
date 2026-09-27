// a webview panel and a webview view, the way chat extensions make theirs: in mme they open in the browser
const vscode = require('vscode');

function page (webview, extUri, name) {
  const js = webview.asWebviewUri(vscode.Uri.joinPath(extUri, 'media', 'main.js'));
  return `<!DOCTYPE html><html><head>
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'nonce-abc'; style-src ${webview.cspSource};">
</head><body><h1>${name}</h1><div id="log"></div><script nonce="abc" src="${js}"></script></body></html>`;
}

function answer (webview, name) {	// what the page says comes back as a message; the page gets an echo
  webview.onDidReceiveMessage((m) => {
    vscode.window.showInformationMessage(name + ' page says: ' + JSON.stringify(m));
    webview.postMessage({echo: m});
  });
}

exports.activate = function (context) {
  // what Go's extension does: util.promisify(execFile) must give {stdout, stderr}, as Node's own
  require('util').promisify(require('child_process').execFile)(process.execPath, ['-e', 'process.stdout.write("42")']).then(
    (r) => {
      if (!r || r.stdout !== '42') vscode.window.showErrorMessage('promisify(execFile) is broken: ' + JSON.stringify(r));
    },
    (e) => vscode.window.showErrorMessage('execFile failed: ' + e.message));
  context.subscriptions.push(vscode.commands.registerCommand('acmeWeb.panel', () => {
    const p = vscode.window.createWebviewPanel('acmeWeb.panel', 'Acme Panel', vscode.ViewColumn.One, {enableScripts: true});
    p.webview.html = page(p.webview, context.extensionUri, 'panel');
    answer(p.webview, 'Panel');
  }));
  context.subscriptions.push(vscode.window.registerWebviewViewProvider('acmeWeb.chat', {
    resolveWebviewView (view) {
      view.webview.options = {enableScripts: true};
      view.webview.html = page(view.webview, context.extensionUri, 'chat view');
      answer(view.webview, 'Chat');
    },
  }));
  // a tree view: a group of fruits; a fruit's command, its context menu, the title's Refresh
  let fruits = ['apple', 'banana'];
  const changed = new vscode.EventEmitter();
  const tree = vscode.window.createTreeView('acmeWeb.fruits', {treeDataProvider: {
    onDidChangeTreeData: changed.event,
    getChildren (el) {
      return el === undefined ? ['Fruits'] : el === 'Fruits' ? fruits : [];
    },
    getTreeItem (el) {
      if (el === 'Fruits') {
        const g = new vscode.TreeItem('Fruits', vscode.TreeItemCollapsibleState.Collapsed);
        g.contextValue = 'group';
        g.iconPath = new vscode.ThemeIcon('folder');
        return g;
      }
      const it = new vscode.TreeItem(el, vscode.TreeItemCollapsibleState.None);
      it.contextValue = 'fruit';
      it.description = 'ripe';
      it.iconPath = new vscode.ThemeIcon('heart');
      it.command = {command: 'acmeWeb.pick', title: 'Pick', arguments: [el]};
      return it;
    },
  }});
  tree.message = undefined;
  context.subscriptions.push(tree,
    vscode.commands.registerCommand('acmeWeb.pick', (f) => vscode.window.showInformationMessage('Picked ' + f)),
    vscode.commands.registerCommand('acmeWeb.eat', (f) => {
      fruits = fruits.filter((x) => x !== f);
      changed.fire('Fruits');
      vscode.window.showInformationMessage('Ate ' + f);
    }),
    vscode.commands.registerCommand('acmeWeb.refresh', () => {
      fruits = ['apple', 'banana', 'cherry'];
      changed.fire();
    }));
  // a task provider: its task is in Run Task, and runs in mme's task terminal
  context.subscriptions.push(vscode.tasks.registerTaskProvider('acme', {
    provideTasks () {
      const t = new vscode.Task({type: 'acme', word: 'hello'}, vscode.TaskScope.Workspace, 'say hello', 'acme',
        new vscode.ShellExecution('echo hello from the acme task'));
      t.group = vscode.TaskGroup.Test;
      return [t];
    },
    resolveTask () {
      return undefined;
    },
  }));
  // a test controller: two tests in this file; its run profile passes one and fails the other
  const ctrl = vscode.tests.createTestController('acme', 'Acme Tests');
  const here = vscode.Uri.joinPath(context.extensionUri, 'extension.js');
  const add = ctrl.createTestItem('adds', 'acme adds', here);
  add.range = new vscode.Range(0, 0, 0, 1);
  const bad = ctrl.createTestItem('fails', 'acme fails', here);
  bad.range = new vscode.Range(1, 0, 1, 1);
  ctrl.items.replace([add, bad]);
  ctrl.createRunProfile('Run', vscode.TestRunProfileKind.Run, (request) => {
    const run = ctrl.createTestRun(request);
    for (const t of request.include) {
      run.started(t);
      run.appendOutput('running ' + t.label + '\r\n');
      if (t.id === 'adds') run.passed(t, 5);
      else run.failed(t, new vscode.TestMessage('expected 4, got 5'), 7);
    }
    run.end();
  }, true);
  context.subscriptions.push(ctrl);
  // semantic tokens: every "several" in plain text is a function (its own legend: the host puts it in its one)
  const legend = new vscode.SemanticTokensLegend(['acmeThing', 'function'], ['loud']);
  context.subscriptions.push(vscode.languages.registerDocumentSemanticTokensProvider({language: 'plaintext'}, {
    provideDocumentSemanticTokens (doc) {
      const b = new vscode.SemanticTokensBuilder(legend);
      for (let i = 0; i < doc.lineCount; i++) {
        const k = doc.lineAt(i).text.indexOf('several');
        if (k >= 0) b.push(i, k, 7, 1, 0);
      }
      return b.build();
    },
  }, legend));
  // a debugger: its configuration provider adds a word, its adapter is written here (an inline one)
  context.subscriptions.push(vscode.debug.registerDebugConfigurationProvider('acme', {
    resolveDebugConfiguration (folder, config) {
      config.greeting = 'hello from the acme debugger';
      return config;
    },
  }));
  context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('acme', {
    createDebugAdapterDescriptor () {
      const out = new vscode.EventEmitter();
      let seq = 1, greeting = '';
      const send = (m) => out.fire(Object.assign({seq: seq++}, m));
      const reply = (req, body) => send({type: 'response', request_seq: req.seq, success: true, command: req.command, body});
      return new vscode.DebugAdapterInlineImplementation({
        onDidSendMessage: out.event,
        handleMessage (m) {
          if (m.command === 'initialize') {
            reply(m, {supportsConfigurationDoneRequest: true});
            send({type: 'event', event: 'initialized'});
          } else if (m.command === 'launch') {
            greeting = m.arguments.greeting;
            reply(m, {});
          } else if (m.command === 'configurationDone') {
            reply(m, {});
            send({type: 'event', event: 'output', body: {category: 'stdout', output: greeting + '\n'}});
          } else if (m.command === 'acmeHello') {	// the extension's customRequest; then a custom event, and the end
            reply(m, {text: 'hi ' + m.arguments.name + ' from the adapter'});
            send({type: 'event', event: 'acmeEvent', body: {n: 1}});
            setTimeout(() => send({type: 'event', event: 'terminated', body: {}}), 300);
          } else if (m.command === 'threads') reply(m, {threads: []});
          else reply(m, {});
        },
        dispose () {},
      });
    },
  }));
  // a tracker counts what the adapter sent; a customRequest and a custom event, said in the debug console
  let seen = 0;
  context.subscriptions.push(vscode.debug.registerDebugAdapterTrackerFactory('acme', {
    createDebugAdapterTracker: () => ({
      onDidSendMessage: () => seen++,
      onWillStopSession: () => vscode.debug.activeDebugConsole.appendLine('tracker: the adapter sent ' + (seen > 3 ? 'its' : 'too few') + ' messages'),
    }),
  }));
  context.subscriptions.push(vscode.debug.onDidStartDebugSession((s) => {
    if (s.type !== 'acme') return;
    seen = 0;
    setTimeout(() => s.customRequest('acmeHello', {name: 'mme'}).then(
      (b) => vscode.debug.activeDebugConsole.appendLine('custom: ' + b.text),
      (e) => vscode.debug.activeDebugConsole.appendLine('custom failed: ' + e.message)), 800);
  }));
  context.subscriptions.push(vscode.debug.onDidReceiveDebugSessionCustomEvent((e) =>
    vscode.debug.activeDebugConsole.appendLine('custom event: ' + e.event + ' ' + JSON.stringify(e.body))));
  context.subscriptions.push(vscode.commands.registerCommand('acmeWeb.debug', () =>
    vscode.debug.startDebugging(undefined, {type: 'acme', name: 'Acme', request: 'launch'})));
  // language models: mme's chat model answers; a tool of its own; a chat participant (@acme)
  context.subscriptions.push(vscode.lm.registerTool('acme_upper', {
    invoke: (o) => new vscode.LanguageModelToolResult([new vscode.LanguageModelTextPart(String(o.input.text).toUpperCase())]),
  }));
  context.subscriptions.push(vscode.commands.registerCommand('acmeWeb.askModel', async () => {
    const [model] = await vscode.lm.selectChatModels({vendor: 'copilot', family: 'gpt-4o'});
    if (!model) {
      vscode.window.showWarningMessage('no model');
      return;
    }
    const r = await model.sendRequest([vscode.LanguageModelChatMessage.User('Say hello')], {});
    let text = '';
    for await (const t of r.text) text += t;
    const tool = await vscode.lm.invokeTool('acme_upper', {input: {text: 'tool ok'}});
    vscode.window.showInformationMessage('model: ' + text.split('\n')[0] + ' | ' + tool.content[0].value);
  }));
  context.subscriptions.push(vscode.chat.createChatParticipant('acme.helper', async (request, ctx, stream) => {
    stream.progress('asking the model');
    const r = await request.model.sendRequest([vscode.LanguageModelChatMessage.User(request.prompt)], {});
    let text = '';
    for await (const t of r.text) text += t;
    stream.markdown(request.command === 'shout' ? text.split('\n')[0].toUpperCase() : text.split('\n')[0]);
    return {};
  }));
  // a notebook kernel: it says what the cell was, and a result of its own
  let order = 0;
  const kernel = vscode.notebooks.createNotebookController('acme-kernel', 'jupyter-notebook', 'Acme Kernel',
    (cells) => {
      for (const cell of cells) {
        const ex = kernel.createNotebookCellExecution(cell);
        ex.executionOrder = ++order;
        ex.start(Date.now());
        ex.replaceOutput([
          new vscode.NotebookCellOutput([vscode.NotebookCellOutputItem.stdout('acme ran: ' + cell.document.getText().split('\n')[0] + '\n')]),
          new vscode.NotebookCellOutput([vscode.NotebookCellOutputItem.text('the acme result ' + order)]),
        ]);
        ex.end(true, Date.now());
      }
    });
  kernel.description = 'runs cells in the extension';
  kernel.supportedLanguages = ['python'];
  context.subscriptions.push(kernel);
  // a custom text editor: what its page posts becomes an edit of mme's document
  context.subscriptions.push(vscode.window.registerCustomEditorProvider('acmeWeb.editor', {
    resolveCustomTextEditor (doc, panel) {
      panel.webview.options = {enableScripts: true};
      panel.webview.html = page(panel.webview, context.extensionUri, 'editor of ' + doc.lineCount + ' lines');
      panel.webview.onDidReceiveMessage(() => {
        const e = new vscode.WorkspaceEdit();
        e.insert(doc.uri, new vscode.Position(0, 0), 'EDITED BY THE PAGE ');
        vscode.workspace.applyEdit(e);
      });
    },
  }));
};
