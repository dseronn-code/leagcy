// Menu no celular.
const botao = document.getElementById('hamburguer');
const menu = document.getElementById('menu');
botao.addEventListener('click', () => {
  const aberto = menu.classList.toggle('aberto');
  botao.setAttribute('aria-expanded', String(aberto));
});
menu.addEventListener('click', (e) => {
  if (e.target.tagName === 'A') { menu.classList.remove('aberto'); botao.setAttribute('aria-expanded', 'false'); }
});

// Fora do Windows, o botão de baixar avisa que o launcher é para Windows (o link continua funcionando).
const windows = /Windows/i.test(navigator.userAgent);
if (!windows) {
  document.querySelectorAll('[data-baixar]').forEach((a) => {
    a.title = 'O Legacy Launcher é para Windows 10 e 11';
  });
}
