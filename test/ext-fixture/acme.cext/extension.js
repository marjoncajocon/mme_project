// an extension for C: it serves the language (onLanguage:c), so mme's own C server gives way to it
const vscode = require('vscode');
exports.activate = function (ctx) {
  ctx.subscriptions.push(vscode.commands.registerCommand('acme.cext.hello', () => vscode.window.showInformationMessage('Hello from C')));
  vscode.window.registerAcmeThing();	// an API VS Code does not have: its page lists it as missing
  console.log('ready for C');	// a line of its own (found by the call stack) on its page
};
