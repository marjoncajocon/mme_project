// the notebook renderer of application/x-acme (web code: it runs in the page the output opens in)
export function activate () {
  return {
    renderOutputItem (item, element) {
      element.innerHTML = '<b style="color:#e8a33d">ACME RENDERER</b> said: <i></i>';
      element.querySelector('i').textContent = item.json().said;
    },
  };
}
