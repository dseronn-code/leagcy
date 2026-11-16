// Atualização dos jogos e do Legacy Engine pelo launcher, baixando só o que mudou (como a Epic/Steam).
//
// Cada Release publica "manifesto-<produto>.json": a versão e a lista de TODOS os arquivos do produto, cada um com
// sha256, tamanho e o endereço de onde baixá-lo (comprimido em gzip). Um arquivo que não mudou continua apontando
// para a Release onde foi publicado pela primeira vez, então cada versão nova só sobe (e cada jogador só baixa) o que
// mudou. O launcher:
//   1. lê o manifesto novo;
//   2. compara com o que está instalado (manifesto-local.json; sem ele, calcula o sha256 dos arquivos na pasta);
//   3. baixa para uma pasta temporária só os arquivos diferentes, conferindo o sha256 de cada um;
//   4. com tudo baixado e conferido, troca os arquivos de uma vez e apaga os que saíram do produto (só os que
//      vieram de um manifesto — saves, cache e configurações do jogador nunca são tocados).
// Uma queda de internet no meio não estraga a instalação: os arquivos só são trocados no fim; o que já foi baixado
// e conferido fica na pasta temporária e é reaproveitado na próxima tentativa.
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const zlib = require('zlib');

const RELEASES = process.env.LEGACY_RELEASES || 'https://github.com/dseronn-code/legacy/releases';
const LOCAL = 'manifesto-local.json';
const TEMP = '.atualizando';

function pedir(url, voltas = 0) {
  return new Promise((ok, falha) => {
    const mod = url.startsWith('http://') ? http : https;
    const req = mod.get(url, { headers: { 'User-Agent': 'Legacy-Launcher' } }, (r) => {
      if ([301, 302, 303, 307, 308].includes(r.statusCode) && voltas < 6) {
        r.resume();
        return pedir(new URL(r.headers.location, url).toString(), voltas + 1).then(ok, falha);
      }
      if (r.statusCode !== 200) {
        r.resume();
        const e = new Error('HTTP ' + r.statusCode);
        e.status = r.statusCode;
        return falha(e);
      }
      ok(r);
    });
    req.on('error', () => falha(new Error('Sem conexão com a internet.')));
    req.setTimeout(60000, () => req.destroy(new Error('A conexão parou de responder.')));
  });
}

async function lerJson(url) {
  const r = await pedir(url);
  let t = '';
  for await (const c of r) t += c;
  return JSON.parse(t);
}

// Manifesto da Release mais nova (null se a Release não tiver: produto sem atualização por arquivos).
async function manifestoNovo(id) {
  try {
    return await lerJson(`${RELEASES}/latest/download/manifesto-${id}.json`);
  } catch (e) {
    if (e.status === 404) return null;
    throw e;
  }
}

function sha256Arquivo(arq) {
  return new Promise((ok, falha) => {
    const h = crypto.createHash('sha256');
    fs.createReadStream(arq).on('data', (c) => h.update(c)).on('end', () => ok(h.digest('hex'))).on('error', falha);
  });
}

function seguro(pasta, rel) {  // nunca escrever fora da pasta do produto (manifesto adulterado)
  const alvo = path.resolve(pasta, rel);
  if (!alvo.startsWith(path.resolve(pasta) + path.sep)) throw new Error('Caminho inválido no manifesto: ' + rel);
  return alvo;
}

// O que está instalado: {caminho: sha256}. Do manifesto local (rápido) ou, sem ele, calculando (1ª atualização
// depois de instalar pelo instalador).
async function instalado(pasta, novo) {
  try {
    const m = JSON.parse(fs.readFileSync(path.join(pasta, LOCAL), 'utf8'));
    const mapa = {};
    for (const a of m.arquivos) mapa[a.caminho] = a.sha256;
    // confere se os arquivos ainda estão lá com o tamanho certo (alguém pode ter apagado ou o antivírus removido)
    for (const a of m.arquivos) {
      try {
        if (fs.statSync(seguro(pasta, a.caminho)).size !== a.tamanho) delete mapa[a.caminho];
      } catch { delete mapa[a.caminho]; }
    }
    return { mapa, anteriores: m.arquivos.map((a) => a.caminho) };
  } catch {
    const mapa = {};
    for (const a of novo.arquivos) {
      const arq = seguro(pasta, a.caminho);
      try {
        if (fs.statSync(arq).size === a.tamanho) mapa[a.caminho] = await sha256Arquivo(arq);
      } catch {}
    }
    return { mapa, anteriores: [] };
  }
}

async function baixarArquivo(a, destino) {
  const r = await pedir(a.url);
  fs.mkdirSync(path.dirname(destino), { recursive: true });
  const parcial = destino + '.parcial';
  const h = crypto.createHash('sha256');
  let n = 0;
  await new Promise((ok, falha) => {
    const g = a.url.endsWith('.gz') ? zlib.createGunzip() : null;
    const saida = fs.createWriteStream(parcial);
    const fonte = g ? r.pipe(g) : r;
    fonte.on('data', (c) => { h.update(c); n += c.length; });
    fonte.on('error', falha);
    r.on('error', falha);
    saida.on('error', falha);
    saida.on('finish', ok);
    fonte.pipe(saida);
  });
  if (n !== a.tamanho || h.digest('hex') !== a.sha256) {
    fs.rmSync(parcial, { force: true });
    throw new Error('Arquivo corrompido no download (' + a.caminho + '). Tente de novo.');
  }
  fs.renameSync(parcial, destino);
}

// O jogo aberto segura o legacy.exe: renomear falha no Windows. Melhor descobrir antes de trocar qualquer arquivo.
function emUso(pasta) {
  const exe = path.join(pasta, 'legacy.exe');
  if (!fs.existsSync(exe)) return false;
  try {
    fs.renameSync(exe, exe + '.teste');
    fs.renameSync(exe + '.teste', exe);
    return false;
  } catch {
    return true;
  }
}

// Quanto falta baixar (para mostrar "Atualizar (12 MB)") — null se não há atualização por arquivos.
async function verificar(id, pasta) {
  const novo = await manifestoNovo(id);
  if (!novo) return null;
  const { mapa } = await instalado(pasta, novo);
  const faltam = novo.arquivos.filter((a) => mapa[a.caminho] !== a.sha256);
  return { versao: novo.versao, arquivos: faltam.length, bytes: faltam.reduce((s, a) => s + (a.comprimido || a.tamanho), 0) };
}

// progresso(fração 0..1, texto)
async function atualizar(id, pasta, progresso = () => {}) {
  const novo = await manifestoNovo(id);
  if (!novo) throw new Error('Esta versão não tem atualização pelo launcher. Use Reinstalar.');
  const { mapa, anteriores } = await instalado(pasta, novo);
  const faltam = novo.arquivos.filter((a) => mapa[a.caminho] !== a.sha256);
  const temp = path.join(pasta, TEMP);
  const total = faltam.reduce((s, a) => s + (a.comprimido || a.tamanho), 0) || 1;
  let feito = 0;
  for (const a of faltam) {
    const destino = seguro(temp, a.caminho);
    let pronto = false;
    if (fs.existsSync(destino)) {  // baixado numa tentativa anterior: reaproveita se estiver íntegro
      pronto = fs.statSync(destino).size === a.tamanho && (await sha256Arquivo(destino)) === a.sha256;
    }
    if (!pronto) await baixarArquivo(a, destino);
    feito += a.comprimido || a.tamanho;
    progresso(feito / total, `Baixando ${faltam.indexOf(a) + 1} de ${faltam.length}`);
  }
  if (emUso(pasta)) throw new Error('Feche o jogo para terminar a atualização (o que foi baixado fica guardado).');
  progresso(1, 'Aplicando');
  for (const a of faltam) {
    const alvo = seguro(pasta, a.caminho);
    fs.mkdirSync(path.dirname(alvo), { recursive: true });
    fs.rmSync(alvo, { force: true });
    fs.renameSync(seguro(temp, a.caminho), alvo);
  }
  const atuais = new Set(novo.arquivos.map((a) => a.caminho));
  for (const c of anteriores) if (!atuais.has(c)) fs.rmSync(seguro(pasta, c), { force: true });
  fs.rmSync(temp, { recursive: true, force: true });
  fs.writeFileSync(path.join(pasta, LOCAL), JSON.stringify({ versao: novo.versao, arquivos: novo.arquivos }));
  fs.writeFileSync(path.join(pasta, 'versao-legacy.txt'), novo.versao);
  return { versao: novo.versao, arquivos: faltam.length, bytes: total };
}

module.exports = { verificar, atualizar, sha256Arquivo, RELEASES };
