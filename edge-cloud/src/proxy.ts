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

// frame の Content-Length を edge と同じ契約で確かめる (edge/src/edge/api.py の _read_frame_body)。
// 無い → 411、十進数でない (負を含む) → 400、2 MiB 超 → 413。どれもコンテナを起こさない。
// Worker → Durable Object → コンテナの経路で Content-Length が保たれることは初回デプロイで確認済み
// (docs/measurements/step6-2026-10-10.md)。
function frameLengthError(request: Request, pathname: string): Response | null {
  if (request.method !== "POST" || !FRAME_PATH.test(pathname)) return null;
  const length = request.headers.get("Content-Length");
  if (length === null) return json({ error: "length_required" }, 411);
  if (!/^[0-9]+$/.test(length)) return json({ error: "invalid_header:content-length" }, 400);
  if (Number(length) > MAX_FRAME_BYTES) return json({ error: "frame_too_large" }, 413);
  return null;
}

export async function handle(request: Request, env: ProxyEnv, forward: Forward): Promise<Response> {
  const { pathname } = new URL(request.url);
  if (!pathname.startsWith("/v1/")) return json({ error: "not_found" }, 404);
  // 認証を通るまでコンテナに触れない (認証なしのリクエストでコンテナを起こさない)
  if (!(await verifyDevice(request, env))) return json({ error: "unauthorized" }, 401);
  // 理由はアクセスログ (status 503 の early) で分かるので、ここでは別のログを出さない
  if (!galleryConfigured(env)) return json({ error: "misconfigured" }, 503);
  const lengthError = frameLengthError(request, pathname);
  if (lengthError) return lengthError;
  return forward(request);
}

// ---- アクセスログ (docs/design/step6-cloud-device.md §3.1) ----

export type RequestKind = "hello" | "frame" | "candidate" | "save" | "photo" | "other";

// URL やセッション ID はログに出さず、種類だけにする。
// review は本文を読まないので retake も "save" に数える (本文を読むと転送する本文を消費してしまう)。
export function requestKind(method: string, pathname: string): RequestKind {
  if (method === "POST" && pathname === "/v1/hello") return "hello";
  if (method === "POST" && FRAME_PATH.test(pathname)) return "frame";
  if (method === "GET" && /^\/v1\/sessions\/[^/]+\/candidate$/.test(pathname)) return "candidate";
  if (method === "POST" && /^\/v1\/sessions\/[^/]+\/review$/.test(pathname)) return "save";
  if (method === "GET" && /^\/v1\/sessions\/[^/]+\/photo$/.test(pathname)) return "photo";
  return "other";
}

export interface AccessLog {
  // early: Worker が自分で返した (404 / 401 / 503 / 411 / 400 / 413 / 500)
  // forward: コンテナの応答を返した (status はコンテナのもの)
  // forward_error: 転送で例外が出て 502 upstream_error を返した
  event: "early" | "forward" | "forward_error";
  kind: RequestKind;
  status: number;
  ms: number;
  colo: string | null;
}

export interface ServeOptions {
  colo?: string | null;
  log?: (line: string) => void;
  now?: () => number;
}

// handle() を包み、全応答でちょうど 1 件のログを出す。
// 鍵・URL・ヘッダ値・本文は出さない (edge の logging_setup.py と同じ方針)。
// ms は応答ヘッダが揃うまで (Workers の時計は I/O の間だけ進むので、early はほぼ 0 になる)。
export async function serve(
  request: Request,
  env: ProxyEnv,
  forward: Forward,
  options: ServeOptions = {},
): Promise<Response> {
  const now = options.now ?? Date.now;
  const log = options.log ?? ((line: string) => console.log(line));
  const started = now();
  let kind: RequestKind = "other";
  let forwarded = false;
  let event: AccessLog["event"] = "early";
  let response: Response;
  try {
    kind = requestKind(request.method, new URL(request.url).pathname);
    response = await handle(request, env, (req) => {
      forwarded = true;
      return forward(req);
    });
    if (forwarded) event = "forward";
  } catch {
    // 例外の中身 (URL やヘッダを含みうる) はログにも応答にも出さない
    if (forwarded) {
      event = "forward_error";
      response = json({ error: "upstream_error" }, 502);
    } else {
      response = json({ error: "internal_error" }, 500);
    }
  }
  const entry: AccessLog = {
    event,
    kind,
    status: response.status,
    ms: Math.max(0, now() - started),
    colo: options.colo ?? null,
  };
  log(JSON.stringify(entry));
  return response;
}
