# カメラ初期化後の無音調査

対象: [PR #1](https://github.com/na2kera/stack-chan-photobooth/pull/1) の device。2026-09-30 時点で、ソース調査とビルドを実施。K151 は USB ポート一覧に見つからず、発音・カメラ・再試行の実機確認は未実施。

## 結論と確認した事実

無音の原因は未確定。内部 I2C の所有権をカメラへ移す実装にはリスクがあるため、通常ビルドでは M5 が所有するバスを SCCB が借りる構成に変更した。これで発音が回復するかは実機で比較する。

- ユーザー提供ログ: `playWav()` は成功し、約 2.46 秒後に再生終了。これは再生タスクの完走を示すが、アンプの有効化や実際の発音を保証しない。
- WAV を Python の `wave` で読み、mono / 16 kHz / 16-bit PCM を確認。announce は 2.428 秒、ピーク 9248、RMS 1841。captured は 0.837 秒、ピーク 8000、RMS 1754。全サンプルがゼロのファイルではない。
- 固定依存 M5Unified 0.2.24 の `src/M5Unified.inl`: `board_M5StackChan` は CoreS3 と同じ分岐。BCK=34、WS=33、DATA=13、I2S=1、`_speaker_enabled_cb_cores3` を設定している。StackChan のスピーカー設定が未対応という仮説は、この版のソースと一致しない。
- 同コールバックは `M5.In_I2C` 経由で AW88298（0x36）の 16-bit レジスタを書き、AW9523（0x58）の 0x02 bit 2 を操作する。AW88298 の書き込み成否は呼び出し元へ伝わらず、コールバックは true を返す。I2C 書き込みが失敗しても `Speaker.begin()` の成功だけでは検出できない。
- `src/utility/Speaker_Class.inl`: 有効化コールバックは実際の `begin()`、無効化は `end()` で呼ばれる。開始済みの `begin()` は早期 return。WAV ごと、または `isPlaying()` が false になるたびにアンプをオン・オフする実装ではない。
- アプリはカメラ初期化前に `speakerOn()` を呼び、以後 Mode::Speaker なら早期 return。従って「カメラ初期化後の各再生開始でアンプ書き込みが失敗する」という説明は、通常の起動経路には当てはまらない。カメラ初期化前からアンプが有効か、初期化後に状態が変わるかを測る必要がある。
- M5Unified の内部 I2C は StackChan でポート1 / SDA=12 / SCL=11。Arduino 同梱 SDK の `esp32s3/sdkconfig` も SCCB の新ドライバ・ポート1を選択している。旧実装は M5 側の `release()` 後に、同じポートをカメラ側で新規作成していた。
- M5 の I2C_Class は M5GFX の `m5gfx::i2c` に処理を委譲する。M5GFX 0.2.31 はハードウェアレジスタを扱う経路を持つため、「release 後は M5 の全 I2C 操作が必ず失敗する」とは断定できない。画面タッチが動いていることも、アンプ書き込みの成功の証拠にはならない。

スピーカーの構成は [StackChan 公式ドキュメント](https://docs.m5stack.com/en/StackChan) とも一致する。類似報告 [M5Unified #303](https://github.com/m5stack/M5Unified/issues/303) は音の歪みを扱っており、本件の無音と同一原因とは判断しない。

## 修正と再利用方針

`esp_camera` の既存バス利用 API を使い、ライブラリの変更やアンプ設定の独自実装は行わない。[esp_camera.c](https://github.com/espressif/esp32-camera/blob/v2.1.4/driver/esp_camera.c) は SDA が -1 のときにだけ `SCCB_Use_Port()` を選ぶため、`sccb_i2c_port` だけ変える方法では共有されない。

通常設定は `pin_sccb_sda = -1` / `pin_sccb_scl = -1` / `sccb_i2c_port = M5.In_I2C.getPort()`。初期化前に IDF の bus handle が存在することを確認し、不在ならカメラ初期化を失敗として扱う。共有時は `M5.In_I2C.release()` を呼ばない。

[SCCB の新ドライバ](https://github.com/espressif/esp32-camera/blob/v2.1.4/driver/sccb-ng.c) は、借りたバスを deinit 時に削除しない。SDK 同梱 `libespressif__esp32-camera.a` の `SCCB_Use_Port` / `SCCB_Deinit` の逆アセンブルでも、所有フラグをクリアする処理と、非所有時にバス削除をスキップする分岐を確認した。このため、初期化失敗・ERROR からの再試行も同じ M5 バスを利用する。

M5GFX の直接操作と SCCB の IDF API は同時アクセスを避ける必要がある。現在のカメラ初期化・再試行・タッチ処理・音声モード切替はメインタスクから順に呼ばれ、フレーム取得は SCCB の設定操作を伴わない。将来、別タスクからセンサー設定や音声モード切替を行う場合は、共有バスへのアクセスを直列化する必要がある。

`Speaker.begin()` が失敗した場合は Off のままにし、ログを出して WAV 再生を開始しない。成功時の音量・WAV・I2S 設定は既存の実装を使う。

## 実機比較手順

`device/platformio.ini` のコメントを外し、以下を別々にビルド・書き込みする。各条件で起動時の3音と、タッチ後の announce / 保存後の captured が聞こえるか記録する。

| 条件 | 有効にするフラグ | 目的 |
| --- | --- | --- |
| カメラなし | `PHOTOBOOTH_AUDIO_DIAGNOSTICS`, `PHOTOBOOTH_NO_CAMERA` | カメラを使わずに発音するか |
| PR #1 相当のバス所有権 | `PHOTOBOOTH_AUDIO_DIAGNOSTICS`, `PHOTOBOOTH_CAMERA_OWN_I2C` | release → カメラ新規バスで比較 |
| 共有バス | `PHOTOBOOTH_AUDIO_DIAGNOSTICS` | M5 のバスを維持した場合との比較 |

テスト音は 1 kHz / 250 ms。ログの順序は `before-camera` → `after-camera` → `after-speaker-restart`。音が重ならないよう各音の終了を待ち、1.5 秒を超えたら停止して timeout を記録する。3音目の前では `Speaker.end()` / `begin()` を明示的に行い、既存の M5Unified コールバックでアンプ設定をやり直す。通常起動の比較を終える際には診断フラグを外すこと。診断中の再初期化は、続くアプリのアンプ状態にも影響する。

ログには board、I2S ピン・レート・音量、AW88298 の 0x00 / 0x01 / 0x04 / 0x05 / 0x06 / 0x0C、AW9523 のスピーカー有効ビット、テスト音の受付・終了時間が出る。レジスタの read 失敗は値 0 と混同せず、エラーとして出す。M5Unified の有効化時の設定は 0x04=0x4040、0x05=0x0008、0x0C=0x0064。0x06 は出力レートに依存する。読み取り成功だけでも、アナログ出力や発音は保証しない。

| 観測 | 判断・次の確認 |
| --- | --- |
| カメラなしでも最初の音が出ない | カメラが原因という仮説を弱める。初期アンプ設定、電源、I2S、実機を確認 |
| before は鳴り、after は鳴らず、restart 後は鳴る | 初期化後のアンプまたは I2S 状態変化を疑う。レジスタ差分と read 成否を確認 |
| 旧バス方式だけ無音、共有方式では鳴る | バス所有権の切替が関与する強い証拠。アンプ書き込み失敗の断定には追加ログが必要 |
| テスト音は鳴るが WAV は鳴らない | WAV 再生経路を重点確認。テスト音の成功は WAV の発音を保証しない |
| レジスタ read が失敗 | 再生成功ログでは見えなかった I2C 通信失敗を確認できる |

`PHOTOBOOTH_NO_CAMERA` で音が出るだけでは、アンプ有効化失敗まで確定しない。カメラ処理が関与することと、その具体的な故障箇所は分けて記録する。

共有方式では、通常フローのプレビュー・画面タッチ・頭部タッチ・サーボに加え、ERROR 画面からのカメラ再試行後にも発音と入力が保たれることを確認する。
