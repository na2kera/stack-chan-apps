// device → edge コンテナの前段。認証と設定の確認をここで済ませ、通ったものだけをコンテナへ渡す。
// cloudflare:workers / @cloudflare/containers に依存しない (vitest で Node のまま試せるようにする)。

const encoder = new TextEncoder();

// edge と同じ上限と応答 (docs/protocol.md「エラー」)。edge 側でも同じ検査をする。
export const MAX_FRAME_BYTES = 2 * 1024 * 1024;
const FRAME_PATH = /^\/v1\/sessions\/[^/]+\/frames$/;

export interface ProxyEnv {
  DEVICE_ID?: string;
  EDGE_DEVICE_KEY?: string;
  GALLERY_URL?: string;
  GALLERY_KEY?: string;
}

export type Forward = (request: Request) => Promise<Response>;

function json(body: unknown, status: number): Response {
  return Response.json(body, { status, headers: { "Cache-Control": "no-store" } });
}

async function digest(value: string): Promise<Uint8Array> {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", encoder.encode(value)));
}

// gallery/src/auth.ts と同じ方式。長さの違いが比較時間に出ないよう SHA-256 をとってから比べる。
async function sameSecret(provided: string, expected: string): Promise<boolean> {
  const [a, b] = await Promise.all([digest(provided), digest(expected)]);
  let difference = 0;
  for (let i = 0; i < b.length; i += 1) {
    difference |= a[i] ^ b[i];
  }
  return difference === 0;
}

export async function verifyDevice(request: Request, env: ProxyEnv): Promise<boolean> {
  // Secret 未設定や空のヘッダでは必ず拒否する (空同士のダイジェストが一致してしまうため)。
  const id = request.headers.get("X-Device-Id");
  const key = request.headers.get("X-Device-Key");
  if (!env.DEVICE_ID || !env.EDGE_DEVICE_KEY || !id || !key) return false;
  const [idOk, keyOk] = await Promise.all([
    sameSecret(id, env.DEVICE_ID),
    sameSecret(key, env.EDGE_DEVICE_KEY),
  ]);
  return idOk && keyOk;
}

// gallery が無いと edge は mock に戻り、スマホで開けない QR を出してしまう。起動させずに止める。
export function galleryConfigured(env: ProxyEnv): boolean {
  return Boolean(env.GALLERY_KEY) && Boolean(env.GALLERY_URL?.startsWith("https://"));
}

// コンテナ (edge) に渡す環境変数。edge/src/edge/config.py の環境変数上書きと対応する。
export function containerEnv(env: ProxyEnv): Record<string, string> {
  return {
    EDGE_DEVICE_ID: env.DEVICE_ID ?? "",
    EDGE_DEVICE_KEY: env.EDGE_DEVICE_KEY ?? "",
    GALLERY_URL: env.GALLERY_URL ?? "",
    GALLERY_KEY: env.GALLERY_KEY ?? "",
  };
}

function frameTooLarge(request: Request, pathname: string): boolean {
  if (request.method !== "POST" || !FRAME_PATH.test(pathname)) return false;
  const length = request.headers.get("Content-Length");
  // 無い・数字でないときは edge に任せる (411 / 400 を edge が返す)
  return length !== null && /^\d+$/.test(length) && Number(length) > MAX_FRAME_BYTES;
}

export async function handle(request: Request, env: ProxyEnv, forward: Forward): Promise<Response> {
  const { pathname } = new URL(request.url);
  if (!pathname.startsWith("/v1/")) return json({ error: "not_found" }, 404);
  // 認証を通るまでコンテナに触れない (認証なしのリクエストでコンテナを起こさない)
  if (!(await verifyDevice(request, env))) return json({ error: "unauthorized" }, 401);
  if (!galleryConfigured(env)) {
    console.log(JSON.stringify({ event: "misconfigured", reason: "gallery" }));
    return json({ error: "misconfigured" }, 503);
  }
  if (frameTooLarge(request, pathname)) return json({ error: "frame_too_large" }, 413);
  return forward(request);
}
