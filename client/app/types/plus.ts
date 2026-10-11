export interface PlusTrack {
  id: string; title: string; artist: string; album: string; duration_ms: number;
  format: string; sample_rate: number; channels: number; size_bytes: number;
  asset_path: string; content_hash: string; status?: 'available' | 'missing' | 'corrupt';
  source_name?: string;
  prepared_gain_db?: number | null;
  tags?: { energy: number; language: string; country: string; genre: string; event_roles: string[]; school_approved: boolean };
  media?: { media_type: 'audio' | 'video'; container: string; duration_ms: number; audio_codec: string; video_codec: string; has_audio: boolean; has_video: boolean; width: number; height: number; frame_rate: number; audio_channels: number; sample_rate: number; audio_proxy_path: string; audio_proxy_key: string; probe_status: string; probe_error: string };
}
export interface PlusBatch {
  id: string; origin: 'upload' | 'server_path'; status: string; cancel_requested: boolean;
  relink_track_id?: string;
  files: { source: { path: string; name: string }; status: string; track_id: string; error: string }[];
}
