# PlatformIO pre スクリプト。
# include/config.h が無ければ include/config.example.h をコピーして、
# 新しい clone でも `pio run` がそのまま通るようにする。
# 既存の config.h は上書きしない（鍵や Wi-Fi 情報が入っているため）。
import shutil
from pathlib import Path

Import("env")  # noqa: F821  (PlatformIO が注入する)

include_dir = Path(env.subst("$PROJECT_INCLUDE_DIR"))  # noqa: F821
config_h = include_dir / "config.h"
example_h = include_dir / "config.example.h"

if not config_h.exists():
    if not example_h.exists():
        raise SystemExit("ensure_config: %s が見つかりません" % example_h)
    shutil.copyfile(example_h, config_h)
    print("ensure_config: %s を %s からコピーしました" % (config_h.name, example_h.name))
