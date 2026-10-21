// Renderiza film.html quadro a quadro no Chromium e monta o MP4 com a trilha.
//   python3 music.py && node render.mjs [--fps 30] [--from 0] [--to 48] [--stills]
import { spawn } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
let playwright;
try {
  playwright = require('playwright');
} catch {
  // Playwright global do ambiente (ex.: /opt/node22/lib/node_modules)
  const { execSync } = await import('node:child_process');
  playwright = require(join(execSync('npm root -g').toString().trim(), 'playwright'));
}

const arg = (name, def) => {
  const i = process.argv.indexOf(`--${name}`);
  return i > 0 ? process.argv[i + 1] : def;
};
const FPS = Number(arg('fps', 30));
const FROM = Number(arg('from', 0));
const TO = Number(arg('to', 48));
const STILLS = process.argv.includes('--stills');
const out = join(here, 'out');
mkdirSync(out, { recursive: true });

const browser = await playwright.chromium.launch({ args: ['--force-color-profile=srgb', '--font-render-hinting=none'] });
const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
await page.goto(pathToFileURL(join(here, 'film.html')).href + '?render');
await page.evaluate(async () => {
  for (const w of [500, 600, 700, 800, 900]) {
    await document.fonts.load(`${w} 80px "Inter Display"`);
    await document.fonts.load(`${w} 40px Inter`);
  }
});

const grab = (t) =>
  page.evaluate((t) => {
    window.render(t);
    return document.getElementById('c').toDataURL('image/jpeg', 0.93).split(',')[1];
  }, t);

if (STILLS) {
  // Quadros-chave para conferência rápida, um por cena.
  const { writeFileSync } = await import('node:fs');
  for (const t of [1.2, 5.5, 7.3, 10.2, 11.3, 12.6, 15, 17, 19, 21, 22.8, 24.8, 27.2, 28.5, 30.4, 31.5, 33.2, 34.5, 37.4, 39, 40.2, 43.5, 45.2]) {
    writeFileSync(join(out, `still-${String(t).padStart(4, '0')}.jpg`), Buffer.from(await grab(t), 'base64'));
  }
  await browser.close();
  console.log('stills em out/');
  process.exit(0);
}

const mp4 = join(out, 'sonora-trailer.mp4');
const ff = spawn('ffmpeg', [
  '-y', '-v', 'error',
  '-f', 'image2pipe', '-framerate', String(FPS), '-i', '-',
  '-ss', String(FROM), '-t', String(TO - FROM), '-i', join(out, 'trilha.wav'),
  '-c:v', 'libx264', '-preset', 'slow', '-crf', '18', '-pix_fmt', 'yuv420p', '-profile:v', 'high', '-movflags', '+faststart',
  '-c:a', 'aac', '-b:a', '256k', '-shortest', mp4,
], { stdio: ['pipe', 'inherit', 'inherit'] });

const total = Math.round((TO - FROM) * FPS);
const started = Date.now();
for (let f = 0; f < total; f++) {
  const buf = Buffer.from(await grab(FROM + f / FPS), 'base64');
  if (!ff.stdin.write(buf)) await new Promise((r) => ff.stdin.once('drain', r));
  if (f % 60 === 0) process.stdout.write(`\r${f}/${total} quadros (${((Date.now() - started) / 1000).toFixed(0)} s)`);
}
ff.stdin.end();
await new Promise((r) => ff.on('close', r));
await browser.close();
console.log(`\n${mp4}`);
