// the demo extension: a bit of every API a real extension uses first
const vscode = require('vscode');

exports.activate = function (context) {
  const out = vscode.window.createOutputChannel('Demo');
  out.appendLine('demo activated');

  const bar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
  bar.text = '$(zap) Demo';
  bar.tooltip = 'The demo extension';
  bar.command = 'demo.sayHello';
  bar.show();

  context.subscriptions.push(out, bar,
    vscode.commands.registerCommand('demo.sayHello', async () => {
      const greeting = vscode.workspace.getConfiguration('demo').get('greeting');
      out.appendLine('said hello');
      const pick = await vscode.window.showInformationMessage(greeting, 'Again', 'Thanks');
      if (pick) out.appendLine('picked ' + pick);
    }),
    vscode.commands.registerCommand('demo.pickColor', async () => {
      const c = await vscode.window.showQuickPick(['red', 'green', 'blue'], {placeHolder: 'A color'});
      if (c) vscode.window.showInformationMessage('You picked ' + c);
    }),
    vscode.languages.registerCompletionItemProvider('plaintext', {
      provideCompletionItems () {
        const a = new vscode.CompletionItem('demoWord', vscode.CompletionItemKind.Keyword);
        a.detail = 'from the demo extension';
        const b = new vscode.CompletionItem('demoSnippet', vscode.CompletionItemKind.Snippet);
        b.insertText = new vscode.SnippetString('demo(${1:arg})');
        return [a, b];
      },
    }),
    // ghost text, the way Supermaven and Blackbox give theirs: after "gho" it offers the rest
    vscode.languages.registerInlineCompletionItemProvider([{language: 'plaintext'}, {language: 'c'}], {	// C: nothing, so Copilot is asked (the fallback)
      provideInlineCompletionItems (doc, pos) {
        const before = doc.lineAt(pos.line).text.slice(0, pos.character);
        if (!before.endsWith('gho')) return [];
        return [new vscode.InlineCompletionItem('ghost from the demo', new vscode.Range(pos.line, pos.character - 3, pos.line, pos.character))];
      },
    }),
    vscode.languages.registerHoverProvider('plaintext', {
      provideHover (doc, pos) {
        const r = doc.getWordRangeAtPosition(pos);
        if (!r) return undefined;
        return new vscode.Hover(new vscode.MarkdownString('**demo hover:** `' + doc.getText(r) + '`'), r);
      },
    }));

  // every TODO in a plain text file is a warning
  const diags = vscode.languages.createDiagnosticCollection('demo');
  context.subscriptions.push(diags);
  const lint = (doc) => {
    if (doc.languageId !== 'plaintext') return;
    const list = [];
    for (let i = 0; i < doc.lineCount; i++) {
      const t = doc.lineAt(i).text;
      let k = t.indexOf('TODO');
      while (k >= 0) {
        const d = new vscode.Diagnostic(new vscode.Range(i, k, i, k + 4), 'a TODO is left here', vscode.DiagnosticSeverity.Warning);
        d.source = 'demo';
        list.push(d);
        k = t.indexOf('TODO', k + 4);
      }
    }
    diags.set(doc.uri, list);
  };
  vscode.workspace.textDocuments.forEach(lint);
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(lint),
    vscode.workspace.onDidChangeTextDocument((e) => lint(e.document)));
};

exports.deactivate = function () {};
