import { buildApp } from './app';
import { config } from './config';
import { LyricsService } from './lyrics';
import { CatalogService } from './providers/catalog';
import { JamendoProvider } from './providers/jamendo';
import { SubsonicProvider } from './providers/subsonic';
import type { CatalogProvider } from './providers/types';
import { rebuildNeighbors } from './recs/cooccurrenceJob';
import { RecommendationService } from './recs/service';
import { memoryRepos } from './repos/memory';
import { postgresRepos } from './repos/postgres';

const providers: CatalogProvider[] = [];
if (config.subsonic) providers.push(new SubsonicProvider(config.subsonic.url, config.subsonic.user, config.subsonic.password));
if (config.jamendoClientId) providers.push(new JamendoProvider(config.jamendoClientId));
if (providers.length === 0) console.warn('Nenhum provider de catálogo configurado (JAMENDO_CLIENT_ID / SUBSONIC_URL). A busca retornará vazio.');

const repos = config.databaseUrl ? postgresRepos(config.databaseUrl) : memoryRepos();
if (!config.databaseUrl) console.warn('DATABASE_URL ausente: usando armazenamento em memória (dados somem ao reiniciar).');

const catalog = new CatalogService(providers);
const app = await buildApp({ repos, catalog, lyrics: new LyricsService(), recs: new RecommendationService(repos, catalog), jwtSecret: config.jwtSecret, logger: true });

// Em produção o job roda separado (npm run job:cooccurrence); aqui roda a cada hora por conveniência.
setInterval(() => void rebuildNeighbors(repos).catch((e) => app.log.error(e)), 3_600_000).unref();

await app.listen({ port: config.port, host: '0.0.0.0' });
