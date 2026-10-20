-- Sonora — esquema PostgreSQL 16+ (com pgvector para embeddings de áudio).
CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION IF NOT EXISTS vector;

CREATE TABLE IF NOT EXISTS users (
  id            uuid PRIMARY KEY DEFAULT gen_random_uuid(),
  email         text UNIQUE NOT NULL,
  display_name  text NOT NULL,
  password_hash text NOT NULL,
  created_at    timestamptz NOT NULL DEFAULT now()
);

-- Log de sincronização (event sourcing). Escopo = 'user:<id>' ou 'playlist:<id>'
-- (playlists colaborativas têm escopo próprio, compartilhado entre colaboradores).
CREATE TABLE IF NOT EXISTS sync_ops (
  seq        bigserial PRIMARY KEY,
  scope      text NOT NULL,
  entity     text NOT NULL,
  entity_id  text NOT NULL,
  field      text NOT NULL,
  value      jsonb,
  hlc        text NOT NULL,
  device_id  text NOT NULL,
  author_id  uuid NOT NULL REFERENCES users(id),
  UNIQUE (scope, entity, entity_id, field, hlc)
);
CREATE INDEX IF NOT EXISTS sync_ops_scope_seq ON sync_ops (scope, seq);

CREATE TABLE IF NOT EXISTS playlist_members (
  playlist_id text NOT NULL,
  user_id     uuid NOT NULL REFERENCES users(id),
  role        text NOT NULL CHECK (role IN ('owner', 'editor', 'follower')),
  PRIMARY KEY (playlist_id, user_id)
);

CREATE TABLE IF NOT EXISTS playlist_invites (
  token       text PRIMARY KEY,
  playlist_id text NOT NULL,
  created_by  uuid NOT NULL REFERENCES users(id),
  expires_at  timestamptz NOT NULL
);

-- Histórico de reprodução (append-only). Particionado por mês em produção.
CREATE TABLE IF NOT EXISTS play_events (
  id                text PRIMARY KEY,           -- gerado no cliente: idempotente em reenvios
  user_id           uuid NOT NULL REFERENCES users(id),
  track_id          text NOT NULL,
  artist_ids        text[] NOT NULL,
  genres            text[] NOT NULL,
  started_at        timestamptz NOT NULL,
  listened_ms       integer NOT NULL CHECK (listened_ms >= 0),
  track_duration_ms integer NOT NULL,
  skipped           boolean NOT NULL,
  source            jsonb NOT NULL,
  device_id         text NOT NULL,
  tz_offset_min     smallint NOT NULL
);
CREATE INDEX IF NOT EXISTS play_events_user_time ON play_events (user_id, started_at DESC);

-- Rollup diário para estatísticas instantâneas (mantido pelo servidor a cada ingestão).
CREATE TABLE IF NOT EXISTS listening_daily (
  user_id     uuid NOT NULL REFERENCES users(id),
  day         date NOT NULL,                    -- dia LOCAL do usuário
  listened_ms bigint NOT NULL DEFAULT 0,
  plays       integer NOT NULL DEFAULT 0,
  PRIMARY KEY (user_id, day)
);

-- Co-escuta global (gerada pelo job), top-K vizinhos por faixa.
CREATE TABLE IF NOT EXISTS track_neighbors (
  track_id    text NOT NULL,
  neighbor_id text NOT NULL,
  score       real NOT NULL,
  PRIMARY KEY (track_id, neighbor_id)
);

-- Embeddings de conteúdo (áudio + metadados) para cold start e rádio.
CREATE TABLE IF NOT EXISTS track_embeddings (
  track_id  text PRIMARY KEY,
  embedding vector(128) NOT NULL
);
CREATE INDEX IF NOT EXISTS track_embeddings_hnsw ON track_embeddings USING hnsw (embedding vector_cosine_ops);

-- Cache de letras (inclui índice full-text para "buscar por letra").
CREATE TABLE IF NOT EXISTS lyrics_cache (
  track_id   text PRIMARY KEY,
  synced     text,
  plain      text,
  fetched_at timestamptz NOT NULL DEFAULT now(),
  tsv        tsvector GENERATED ALWAYS AS (to_tsvector('simple', coalesce(plain, ''))) STORED
);
CREATE INDEX IF NOT EXISTS lyrics_tsv ON lyrics_cache USING gin (tsv);
