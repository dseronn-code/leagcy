/**
 * Parser da busca avançada. Aceita texto livre + filtros:
 *   artist:"Milton Nascimento" album:clube genre:mpb year:1970-1979
 *   lyrics:"travessia" popularity:>70 quality:hires type:track
 *   -genre:funk (negação)
 */

export type SearchType = 'track' | 'album' | 'artist' | 'playlist';

export interface SearchQuery {
  text: string;
  artist?: string;
  album?: string;
  genres: string[];
  excludeGenres: string[];
  lyrics?: string;
  yearFrom?: number;
  yearTo?: number;
  minPopularity?: number;
  maxPopularity?: number;
  quality?: 'lossless' | 'hires' | 'spatial';
  types: SearchType[];
}

const TOKEN = /(-?)(\w+):("([^"]*)"|\S+)|"([^"]*)"|(\S+)/g;

export function parseSearch(input: string): SearchQuery {
  const q: SearchQuery = { text: '', genres: [], excludeGenres: [], types: [] };
  const free: string[] = [];

  for (const m of input.matchAll(TOKEN)) {
    const [, neg, rawKey, rawVal, quoted, freeQuoted, freeWord] = m;
    if (!rawKey) {
      free.push((freeQuoted ?? freeWord)!);
      continue;
    }
    const key = rawKey.toLowerCase();
    const val = (quoted ?? rawVal)!;
    switch (key) {
      case 'artist':
      case 'album':
      case 'lyrics':
        q[key] = val;
        break;
      case 'genre':
        (neg ? q.excludeGenres : q.genres).push(val.toLowerCase());
        break;
      case 'year': {
        const r = /^(\d{4})(?:-(\d{4}))?$/.exec(val);
        if (r) {
          q.yearFrom = Number(r[1]);
          q.yearTo = Number(r[2] ?? r[1]);
        } else free.push(m[0]);
        break;
      }
      case 'popularity': {
        const p = /^([<>]=?)?(\d{1,3})$/.exec(val);
        if (!p) {
          free.push(m[0]);
          break;
        }
        const n = Number(p[2]);
        if (p[1]?.startsWith('<')) q.maxPopularity = n;
        else q.minPopularity = n;
        break;
      }
      case 'quality':
        if (val === 'lossless' || val === 'hires' || val === 'spatial') q.quality = val;
        break;
      case 'type':
        for (const t of val.split(',')) if (['track', 'album', 'artist', 'playlist'].includes(t)) q.types.push(t as SearchType);
        break;
      default:
        free.push(m[0]);
    }
  }
  q.text = free.join(' ').trim();
  return q;
}

/** Normaliza para comparação: minúsculas e sem acentos ("Música" ~ "musica"). */
export function normalize(s: string): string {
  return s.normalize('NFD').replace(/\p{Diacritic}/gu, '').toLowerCase().trim();
}
