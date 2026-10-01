export interface PlusTrack {
  id: string; title: string; artist: string; album: string; duration_ms: number;
  format: string; sample_rate: number; channels: number; size_bytes: number;
  asset_path: string; content_hash: string; status?: 'available' | 'missing' | 'corrupt';
}
export interface PlusBatch {
  id: string; origin: 'upload' | 'server_path'; status: string; cancel_requested: boolean;
  relink_track_id?: string;
  files: { source: { path: string; name: string }; status: string; track_id: string; error: string }[];
}
