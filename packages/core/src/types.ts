/**
 * Modelo de domínio compartilhado entre app mobile e servidor.
 * Todos os IDs são strings opacas; `provider` identifica a origem licenciada do item.
 */

export type ID = string;

/** Codecs suportados, do mais fiel para o menos fiel. */
export type Codec = 'flac' | 'alac' | 'wav' | 'eac3-joc' | 'opus' | 'aac' | 'mp3';

export interface AudioSource {
  url: string;
  codec: Codec;
  /** kbps (para lossless é a média aproximada). */
  bitrate: number;
  sampleRateHz: number;
  bitDepth?: number;
  channels: number;
  /** true para Dolby Atmos / áudio espacial (E-AC-3 JOC). */
  spatial?: boolean;
  /**
   * Catálogo licenciado: `url` não vem nos metadados; o app pede uma URL
   * assinada e de curta duração em /tracks/:id/stream no momento de tocar.
   */
  requiresAuthorization?: boolean;
  /** Ganho ReplayGain da faixa em dB (normalização). */
  trackGainDb?: number;
  albumGainDb?: number;
  /** Pico da faixa (0..1) para evitar clipping ao aplicar ganho. */
  trackPeak?: number;
}

export interface ArtistRef {
  id: ID;
  name: string;
}

export interface Track {
  id: ID;
  provider: string;
  title: string;
  artists: ArtistRef[];
  albumId: ID;
  albumTitle: string;
  artworkUrl?: string;
  durationMs: number;
  trackNumber?: number;
  discNumber?: number;
  year?: number;
  genres: string[];
  explicit?: boolean;
  /** 0..100 */
  popularity?: number;
  isrc?: string;
  /** Código UPC/EAN do lançamento (exigido nos relatórios de uso). */
  upc?: string;
  /** Gravadora/distribuidora detentora do fonograma. */
  licensor?: string;
  /** Territórios ISO 3166-1 alfa-2 liberados; ausente = sem restrição (CC/acervo próprio). */
  territories?: string[];
  sources: AudioSource[];
  hasSyncedLyrics?: boolean;
}

export type AlbumType = 'album' | 'single' | 'ep' | 'compilation' | 'live';

export interface Album {
  id: ID;
  provider: string;
  title: string;
  artists: ArtistRef[];
  type: AlbumType;
  year?: number;
  releaseDate?: string;
  artworkUrl?: string;
  genres: string[];
  trackIds: ID[];
  label?: string;
  /** Melhor qualidade disponível no álbum (para badge "Hi-Res"). */
  bestQuality?: Pick<AudioSource, 'codec' | 'sampleRateHz' | 'bitDepth' | 'spatial'>;
}

export interface Artist {
  id: ID;
  provider: string;
  name: string;
  imageUrl?: string;
  genres: string[];
  bio?: string;
  monthlyListeners?: number;
  relatedArtistIds?: ID[];
}

export interface ArtistPage {
  artist: Artist;
  topTracks: Track[];
  albums: Album[];
  singles: Album[];
  compilations: Album[];
  related: Artist[];
}

/** `spotify`: espelho somente-leitura de uma playlist do Spotify, sincronizado pelo servidor. */
export type PlaylistKind = 'user' | 'collaborative' | 'smart' | 'algorithmic' | 'spotify';

export interface PlaylistEntry {
  /** ID da entrada (não da faixa): permite a mesma faixa duas vezes. */
  entryId: ID;
  trackId: ID;
  /** Chave de ordenação fracionária (ver fractionalIndex.ts). */
  position: string;
  addedBy: ID;
  addedAt: number;
}

export interface Playlist {
  id: ID;
  ownerId: ID;
  name: string;
  description?: string;
  artworkUrl?: string;
  kind: PlaylistKind;
  collaboratorIds: ID[];
  isPublic: boolean;
  entries: PlaylistEntry[];
  /** Regras para playlists inteligentes. */
  smartRules?: SmartRules;
  folderId?: ID;
  updatedAt: number;
}

export interface SmartRules {
  match: 'all' | 'any';
  rules: SmartRule[];
  limit?: number;
  sort?: 'recent' | 'playCount' | 'random' | 'popularity';
}

export type SmartRule =
  | { field: 'genre'; op: 'is' | 'isNot'; value: string }
  | { field: 'year'; op: 'between'; value: [number, number] }
  | { field: 'artist'; op: 'is' | 'isNot'; value: ID }
  | { field: 'playCount'; op: 'gte' | 'lte'; value: number }
  | { field: 'liked'; op: 'is'; value: boolean }
  | { field: 'lastPlayedDays'; op: 'gte' | 'lte'; value: number }
  | { field: 'quality'; op: 'is'; value: 'lossless' | 'hires' | 'spatial' };

/** Um evento de escuta. Base de todas as estatísticas e recomendações. */
export interface PlayEvent {
  id: ID;
  trackId: ID;
  artistIds: ID[];
  genres: string[];
  /** epoch ms do início. */
  startedAt: number;
  /** Tempo efetivamente ouvido (exclui pausas e seeks). */
  listenedMs: number;
  trackDurationMs: number;
  /** Pulou antes de 30 s ou 50%? */
  skipped: boolean;
  source: PlaySource;
  deviceId: string;
  /** Fuso do usuário no momento, para agrupar por dia local. */
  tzOffsetMin: number;
}

export type PlaySource =
  | { type: 'album'; id: ID }
  | { type: 'playlist'; id: ID }
  | { type: 'artist'; id: ID }
  | { type: 'radio'; seed: ID }
  | { type: 'search' }
  | { type: 'library' }
  | { type: 'queue' };

export type RepeatMode = 'off' | 'all' | 'one';

export interface AudioPreferences {
  wifiQuality: StreamQuality;
  cellularQuality: StreamQuality;
  downloadQuality: StreamQuality;
  normalization: 'off' | 'track' | 'album' | 'auto';
  /** Alvo de loudness em LUFS. */
  normalizationTargetLufs: number;
  gapless: boolean;
  crossfadeMs: number;
  preferSpatial: boolean;
  /** Android: tenta saída bit-perfect para DAC USB (Android 14+). */
  bitPerfectUsb: boolean;
  playbackSpeed: number;
}

export type StreamQuality = 'data-saver' | 'high' | 'lossless' | 'hires' | 'max';
