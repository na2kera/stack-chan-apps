import { verifyGalleryKey } from "./auth";
import { renderExpiredPage, renderNotFoundPage, renderPhotoPage } from "./pages";
import { shareIntentUrl } from "./share";
import {
  deleteExpired,
  getPhoto,
  isExpired,
  putPhoto,
  type Env,
  type PhotoMetadata,
} from "./store";
import { isToken } from "./token";

const MAX_PHOTO_BYTES = 5 * 1024 * 1024;
const UUID_V4 = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const PHOTO_HEADERS = {
  "Cache-Control": "no-store",
  "Referrer-Policy": "no-referrer",
  "X-Robots-Tag": "noindex, nofollow",
  "Content-Security-Policy": "default-src 'none'; img-src 'self'; style-src 'unsafe-inline'",
};

function json(body: unknown, status: number): Response {
  return Response.json(body, { status, headers: { "Cache-Control": "no-store" } });
}

function html(body: string, status: number): Response {
  return new Response(body, {
    status,
    headers: { ...PHOTO_HEADERS, "Content-Type": "text/html; charset=utf-8" },
  });
}

function expiresAtJst(iso: string): string {
  const date = new Date(iso);
  const offset = new Date(date.getTime() + 9 * 60 * 60_000).toISOString().replace("Z", "+09:00");
  return offset;
}

function publicOrigin(request: Request, env: Env): string {
  return (env.PUBLIC_BASE_URL || new URL(request.url).origin).replace(/\/$/, "");
}

function uploadBody(request: Request, env: Env, token: string, metadata: PhotoMetadata) {
  const origin = publicOrigin(request, env);
  return {
    photo_id: token,
    token,
    photo_url: `${origin}/p/${token}`,
    share_url: `${origin}/share/x`,
    expires_at: expiresAtJst(metadata.expires_at),
  };
}

async function upload(request: Request, env: Env): Promise<Response> {
  if (!(await verifyGalleryKey(request.headers.get("X-Gallery-Key"), env.GALLERY_KEY))) {
    return json({ error: "unauthorized" }, 401);
  }
  if ((request.headers.get("Content-Type") ?? "").split(";", 1)[0].trim() !== "image/jpeg") {
    return json({ error: "unsupported_media_type" }, 415);
  }
  const sessionId = request.headers.get("X-Session-Id") ?? "";
  if (!UUID_V4.test(sessionId)) return json({ error: "invalid_session_id" }, 400);
  const capturedAt = request.headers.get("X-Captured-At") ?? "";
  if (!capturedAt || Number.isNaN(Date.parse(capturedAt))) {
    return json({ error: "invalid_captured_at" }, 400);
  }
  const declaredLength = request.headers.get("Content-Length");
  if (declaredLength && /^\d+$/.test(declaredLength) && Number(declaredLength) > MAX_PHOTO_BYTES) {
    return json({ error: "photo_too_large" }, 413);
  }
  const jpeg = await request.arrayBuffer();
  if (jpeg.byteLength > MAX_PHOTO_BYTES) return json({ error: "photo_too_large" }, 413);
  if (jpeg.byteLength < 2 || new Uint8Array(jpeg, 0, 2)[0] !== 0xff || new Uint8Array(jpeg, 0, 2)[1] !== 0xd8) {
    return json({ error: "invalid_jpeg" }, 415);
  }
  const ttlMinutes = Number(env.TTL_MINUTES);
  if (!Number.isFinite(ttlMinutes) || ttlMinutes <= 0) throw new Error("invalid TTL_MINUTES");
  const photo = await putPhoto(env.PHOTOS, sessionId, jpeg, capturedAt, ttlMinutes);
  return json(uploadBody(request, env, photo.token, photo.metadata), photo.created ? 201 : 200);
}

function downloadFilename(capturedAt: string): string {
  const compact = new Date(capturedAt).toISOString().replace(/[-:]/g, "").slice(0, 15);
  return `stackchan-${compact}Z.jpg`;
}

async function photo(request: Request, env: Env, token: string, jpeg: boolean): Promise<Response> {
  if (!isToken(token)) return html(renderNotFoundPage(), 404);
  const stored = await getPhoto(env.PHOTOS, token);
  if (stored === null) return html(renderNotFoundPage(), 404);
  if (isExpired(stored.metadata)) return html(renderExpiredPage(), 410);
  if (!jpeg) return html(renderPhotoPage(token, stored.metadata), 200);

  const headers = new Headers(PHOTO_HEADERS);
  stored.object.writeHttpMetadata(headers);
  headers.set("Content-Type", "image/jpeg");
  const disposition = new URL(request.url).searchParams.get("download") === "1" ? "attachment" : "inline";
  headers.set("Content-Disposition", `${disposition}; filename="${downloadFilename(stored.metadata.captured_at)}"`);
  return new Response(stored.object.body, { headers });
}

async function fetchHandler(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  if (request.method === "POST" && url.pathname === "/internal/photos") return upload(request, env);
  if (request.method === "GET" && url.pathname === "/share/x") {
    return Response.redirect(shareIntentUrl(env.SHARE_TEXT), 302);
  }
  if (request.method === "GET" && url.pathname.startsWith("/p/")) {
    const part = url.pathname.slice(3);
    const jpeg = part.endsWith(".jpg");
    const token = jpeg ? part.slice(0, -4) : part;
    return photo(request, env, token, jpeg);
  }
  return new Response("Not Found", { status: 404 });
}

export default {
  fetch: fetchHandler,
  async scheduled(_controller: ScheduledController, env: Env, _ctx: ExecutionContext): Promise<void> {
    const deleted = await deleteExpired(env.PHOTOS);
    console.log(JSON.stringify({ event: "gallery_sweep", deleted }));
  },
} satisfies ExportedHandler<Env>;
