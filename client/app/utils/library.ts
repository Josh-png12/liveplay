import type { PlusTrack } from '../types/plus';
export const librarySearch = (value: string) => value.normalize('NFKD').replace(/\p{Diacritic}/gu, '').toLocaleLowerCase();
export function filterLibrary(tracks: PlusTrack[], search: string, format: string, status: string,
  sort: keyof PlusTrack, descending = false): PlusTrack[] {
  const query = librarySearch(search);
  return tracks.filter(track => (!format || track.format === format) && (!status || (track.status || 'available') === status)
    && librarySearch(`${track.title} ${track.artist} ${track.album}`).includes(query))
    .sort((a, b) => {
      const x = a[sort], y = b[sort];
      const order = typeof x === 'number' && typeof y === 'number' ? x - y : String(x ?? '').localeCompare(String(y ?? ''));
      return (descending ? -1 : 1) * (order || a.id.localeCompare(b.id));
    });
}
export const completedFiles = (batch: { files: {status: string}[] }) => batch.files.filter(file => file.status !== 'queued').length;
