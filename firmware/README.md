
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```

## Photobooth fork (na2kera/StackChan `photobooth` ブランチ)

このフォークは純正ファームに写真撮影アプリ `main/apps/app_photobooth/` を足したもの。
設計は親リポジトリ (photobooth) の `docs/design/fw-app-step1.md`。upstream との差分は次だけにしている。

| 変更 | 場所 |
| --- | --- |
| アプリ本体 (ランチャーに「Photobooth」として最後に並ぶ) | `main/apps/app_photobooth/` |
| アプリの登録 | `main/apps/apps.h` の include 1 行、`main/main.cpp` の `installApp` 1 行 |
| セリフ WAV の埋め込み | `main/CMakeLists.txt` の `PHOTOBOOTH_VOICES` (`EMBED_FILES`) |
| 起動時の自動ファーム更新を止める | `patches/xiaozhi-esp32.patch` (`xiaozhi-esp32/main/application.cc` の `CheckNewVersion()`) |

### 自動更新を止めている理由と範囲

純正は AI エージェント起動時の版チェック (`Application::CheckNewVersion()`) で新しい版があると
自動で書き換えて再起動するため、このビルド (と app_photobooth) が純正に戻ってしまう。
パッチでは「新版があっても `UpgradeFirmware()` を呼ばずにログを出すだけ」にしている。
版チェック自体は活性化と MQTT / WebSocket 設定の取得を兼ねるので残している (AI エージェントはそのまま動く)。

- 手動更新は残している: SETUP の更新 (`Hal::updateFirmware`) や AI に頼む `self.upgrade_firmware` を実行すると、
  純正ファームに置き換わって app_photobooth は消える。戻すときはこのリポジトリから書き込み直す。
- `fetch_repos.py` は `xiaozhi-esp32` を clone したあと `patches/xiaozhi-esp32.patch` を当てる。
  パッチを変えるときは `xiaozhi-esp32/` で編集してから `git -C xiaozhi-esp32 diff > patches/xiaozhi-esp32.patch`。
  (`xiaozhi-esp32/` と `components/` は .gitignore 済みで、コミットしない)
- 既に `xiaozhi-esp32/` がある (前の版のパッチが当たっている) 環境では、`fetch_repos.py` が
  「cannot be applied cleanly, skipped」と出してパッチを当てない。その場合は
  `git -C xiaozhi-esp32 checkout -- .` で戻してから `python3 ./fetch_repos.py` を実行し直す。
  当たっているかは `grep -n "photobooth fork" xiaozhi-esp32/main/application.cc` で確認できる。

### 素材の作り直し

| 素材 | コマンド | 依存 |
| --- | --- | --- |
| セリフ (`assets/voice/*.wav`, 24 kHz / mono / 16-bit) | `main/apps/app_photobooth/tools/make_voice.sh` | macOS `say` (Kyoko), ffmpeg |
| 日本語フォント (`assets/pb_font_*.c`) | `main/apps/app_photobooth/tools/gen_font.sh` | python3, npx (lv_font_conv 1.5.3)。`idf.py build` 済み (managed_components が必要) |
| ランチャーのアイコン (`assets/icon_photobooth.c`) | `python3 main/apps/app_photobooth/tools/make_icon.py` | Pillow |

画面の文言は `main/apps/app_photobooth/view/strings.h` にまとめてある。文言を変えたら `gen_font.sh` で
フォントを作り直す (純正同梱の `font_puhui_basic_20_4` には一部のかな・漢字が無いため、
同じ PuHuiTi 系の `puhui-common.ttf` から使う文字だけを切り出している)。

### 書き込みと復旧

```bash
idf.py build
idf.py flash monitor
```

純正に戻すときは M5Burner で StackChan の純正ファームを書き込む (従来どおり)。
