// Copia o jogo para a pasta www do aplicativo.
// O Three.js vai junto dentro do APK, para o 3D funcionar mesmo sem internet.
const fs = require('fs');
const path = require('path');

const www = path.join(__dirname, '..', 'www');
fs.rmSync(www, { recursive: true, force: true });
fs.mkdirSync(www, { recursive: true });

const cdn = 'https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js';
const html = fs.readFileSync(path.join(__dirname, '..', '..', 'index.html'), 'utf8');
if (!html.includes(cdn)) throw new Error('Endereço do Three.js não encontrado no index.html');
fs.writeFileSync(path.join(www, 'index.html'), html.replace(cdn, 'three.min.js'));
fs.copyFileSync(require.resolve('three/build/three.min.js'), path.join(www, 'three.min.js'));

console.log('Pasta www pronta.');
