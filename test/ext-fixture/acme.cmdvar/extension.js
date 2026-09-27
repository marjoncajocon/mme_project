// commands that give a value (not in the palette: registered, not contributed), the way
// cmake.launchTargetPath or python.interpreterPath are used as ${command:id} in tasks.json
const vscode = require('vscode');

exports.activate = function (context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('cmdvar.buildName', () => 'from-the-extension'),
    vscode.commands.registerCommand('cmdvar.number', () => 42));
};
