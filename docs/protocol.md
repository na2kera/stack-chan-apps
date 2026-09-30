# Device–Edge 通信契約

ステップ2で確定する。現時点では `docs/spec.md` §8 のイベント表が正。

device 側のインターフェースは `device/src/edge/edge_client.h` に先行して定義し、イベント名と必須項目は spec §8 と一致させる。

| 方向 | イベント | 必須項目 |
| --- | --- | --- |
| Device→Edge | `hello` | `device_id`, `protocol_version` |
| Device→Edge | `audio_clip` | `device_id`, `seq`, `sample_rate`, `pcm_bytes` |
| Device→Edge | `session_start` | `session_id`, `started_at` |
| Device→Edge | `frame` | `session_id`, `frame_id`, `capture_monotonic_ms`, `format`, `width`, `height`, 画像バイト |
| Edge→Device | `frame_result` | `session_id`, `frame_id`, `face_count`, `target_face_count`, `all_eyes_open`, `all_smiling`, `servo_target`, `accepted` |
| Device→Edge | `session_timeout` | `session_id` |
| Device→Edge | `review_decision` | `session_id`, `save` / `retake` |
| Edge→Device | `photo_ready` | `session_id`, `photo_url`, `expires_at`, `share_url` |
| Device→Edge | `session_cancel` | `session_id` |

トランスポート（HTTP + WebSocket か、VAD で区切った短いクリップの POST か）、認証（device_id + 共有鍵）、フレームの形式（JPEG か RGB565 か）はステップ2の実機計測で決めてここに書く。
