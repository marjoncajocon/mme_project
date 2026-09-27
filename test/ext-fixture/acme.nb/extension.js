// a notebook format of its own: {"cells": [{"code": true, "text": "..."}]}; its kernel answers a cell in capitals
const vscode = require('vscode');
exports.activate = function (ctx) {
  ctx.subscriptions.push(vscode.workspace.registerNotebookSerializer('acme-notebook', {
    deserializeNotebook (bytes) {
      const j = JSON.parse(Buffer.from(bytes).toString('utf8') || '{"cells": []}');
      return new vscode.NotebookData(j.cells.map((c) => new vscode.NotebookCellData(c.code ? vscode.NotebookCellKind.Code : vscode.NotebookCellKind.Markup,
        c.text, c.code ? 'plaintext' : 'markdown')));
    },
    serializeNotebook (data) {
      const cells = data.cells.map((c) => ({code: c.kind === vscode.NotebookCellKind.Code, text: c.value}));
      return Buffer.from(JSON.stringify({cells, saved: 'by acme.nb'}, null, 1));
    },
  }, {transientOutputs: true}));
  const ctl = vscode.notebooks.createNotebookController('acme-nb-kernel', 'acme-notebook', 'Acme Shout');
  ctl.executeHandler = (cells) => {
    for (const cell of cells) {
      const ex = ctl.createNotebookCellExecution(cell);
      ex.start(Date.now());
      ex.replaceOutput([new vscode.NotebookCellOutput([vscode.NotebookCellOutputItem.text(cell.document.getText().toUpperCase())]),
        new vscode.NotebookCellOutput([vscode.NotebookCellOutputItem.json({said: cell.document.getText()}, 'application/x-acme')])]);	// its renderer draws it
      ex.end(true, Date.now());
    }
  };
  ctx.subscriptions.push(ctl);
};
