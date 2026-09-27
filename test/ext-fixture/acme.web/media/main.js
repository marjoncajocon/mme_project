// the page's script: it says it is there, and shows what the extension sends
const vscode = acquireVsCodeApi();
window.addEventListener('message', (e) => {
  const d = document.createElement('div');
  d.textContent = JSON.stringify(e.data);
  document.getElementById('log').appendChild(d);
});
vscode.postMessage({ready: true});
