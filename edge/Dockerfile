# edge を Cloudflare Containers で動かすイメージ (docs/design/step5-edge-cloud.md)。
# ビルドコンテキストは edge/。Cloudflare Containers は linux/amd64 だけを動かす
# (mediapipe も Linux 向けは x86_64 の wheel しか無いので、arm64 でビルドすると uv sync で失敗する)。
#   docker build --platform linux/amd64 -t stackchan-edge edge
# 鍵と gallery の接続先はイメージに入れず、実行時に環境変数で渡す
# (EDGE_DEVICE_KEY / EDGE_DEVICE_ID / GALLERY_KEY / GALLERY_URL。edge/README.md の「設定」)。

# slim の既定 Debian が変わると下の apt パッケージ名が変わるので、版 (trixie) まで固定する
FROM python:3.11-slim-trixie

# import mediapipe に必要な共有ライブラリだけを入れる (wheel の DT_NEEDED から洗い出した):
# - libmediapipe.so が libEGL.so.1 / libGLESv2.so.2 を要求する (CPU 推論でもロード時に必要)
# - mediapipe が import 時に読む cv2 (opencv-contrib-python) が libGL.so.1 / libglib-2.0 /
#   libgthread-2.0 / libxcb.so.1 を要求する。X11 の残り (libSM 等) は GUI を開くときだけ
# - sounddevice は PortAudio が無くても mediapipe 側で例外を握りつぶすので入れない
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libegl1 \
        libgl1 \
        libgles2 \
        libglib2.0-0t64 \
        libxcb1 \
    && rm -rf /var/lib/apt/lists/*

# CI (.github/workflows/ci.yml) と同じ uv
COPY --from=ghcr.io/astral-sh/uv:0.9.9 /uv /bin/uv
ENV UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy \
    UV_NO_CACHE=1 \
    UV_PYTHON_DOWNLOADS=never \
    UV_PYTHON=/usr/local/bin/python3.11

WORKDIR /app

# 依存だけ先に入れる (ソースを変えてもこの層はキャッシュが効く)
COPY pyproject.toml uv.lock .python-version ./
RUN uv sync --locked --no-dev --no-install-project

# モデルをイメージに焼き込む (起動のたびに取りに行かない)。models/face_landmarker.task
# 取得元の中身が変わったらビルドを失敗させる (float16/1、3,758,596 バイト)
COPY tools/download_model.py tools/download_model.py
RUN python tools/download_model.py \
    && echo "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff  models/face_landmarker.task" \
        | sha256sum -c -

# 本体。editable のまま入れる (config.py が /app/config.toml と /app/models を
# ソースの位置から探すため。--no-editable にすると site-packages を基準にしてしまう)
COPY README.md ./
COPY src ./src
RUN uv sync --locked --no-dev

# 調整値はコミット済みの config.example.toml を正とする。秘密は環境変数だけで渡す
COPY config.example.toml config.toml

RUN useradd --system --uid 10001 --no-create-home --shell /usr/sbin/nologin edge
USER edge

# EDGE_REQUIRE_DEVICE_KEY=1: EDGE_DEVICE_KEY を渡し忘れたら例の鍵 (change-me) で listen せず終了する
ENV PATH="/app/.venv/bin:$PATH" \
    EDGE_REQUIRE_DEVICE_KEY=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    MPLBACKEND=Agg \
    MPLCONFIGDIR=/tmp/matplotlib

EXPOSE 8765
CMD ["edge", "--host", "0.0.0.0", "--port", "8765"]
