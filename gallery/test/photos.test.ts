import { env, SELF } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";

const KEY = "test-gallery-key";
const SESSION = "123e4567-e89b-42d3-a456-426614174000";
const CAPTURED = "2026-10-01T05:30:00Z";
const JPEG = new Uint8Array([0xff, 0xd8, 0xff, 0xd9]);

function headers(key = KEY, session = SESSION): HeadersInit {
  return {
    "Content-Type": "image/jpeg",
    "X-Gallery-Key": key,
    "X-Session-Id": session,
    "X-Captured-At": CAPTURED,
  };
}

async function createPhoto() {
  const response = await SELF.fetch("https://gallery.test/internal/photos", {
    method: "POST",
    headers: headers(),
    body: JPEG,
  });
  return { response, body: await response.json<Record<string, string>>() };
}

describe("POST /internal/photos", () => {
  it("201 で 128-bit token と公開 URL を返す", async () => {
    const { response, body } = await createPhoto();
    expect(response.status).toBe(201);
    expect(body.photo_id).toBe(body.token);
    expect(body.token).toMatch(/^[A-Za-z0-9_-]{22}$/);
    expect(body.photo_url).toBe(`https://gallery.test/p/${body.token}`);
    expect(body.share_url).toBe("https://gallery.test/share/x");
    expect(body.expires_at).toMatch(/\+09:00$/);
  });

  it("同じ session_id の再送を 200 で冪等に返す", async () => {
    const first = await createPhoto();
    const second = await createPhoto();
    expect(second.response.status).toBe(200);
    expect(second.body).toEqual(first.body);
  });

  it("同じ session_id の並行アップロードでも写真は 1 件だけ", async () => {
    const session = "223e4567-e89b-42d3-a456-426614174000";
    const send = () =>
      SELF.fetch("https://gallery.test/internal/photos", { method: "POST", headers: headers(KEY, session), body: JPEG });
    const responses = await Promise.all([send(), send(), send()]);
    const bodies = await Promise.all(responses.map((r) => r.json<Record<string, string>>()));
    expect(responses.map((r) => r.status).sort()).toEqual([200, 200, 201]);
    expect(new Set(bodies.map((b) => b.token)).size).toBe(1);
    const listed = await env.PHOTOS.list({ prefix: "photos/", include: ["customMetadata"] });
    expect(listed.objects.filter((o) => o.customMetadata?.session_id === session)).toHaveLength(1);
  });

  it("空の鍵ヘッダを 401 にする", async () => {
    const response = await SELF.fetch("https://gallery.test/internal/photos", {
      method: "POST",
      headers: headers(""),
      body: JPEG,
    });
    expect(response.status).toBe(401);
  });

  it("EOI で終わらない JPEG を 415 にする", async () => {
    const response = await SELF.fetch("https://gallery.test/internal/photos", {
      method: "POST",
      headers: headers(KEY, "323e4567-e89b-42d3-a456-426614174000"),
      body: new Uint8Array([0xff, 0xd8, 0xff, 0xe0, 0x00]),
    });
    expect(response.status).toBe(415);
  });

  it("鍵違いを 401 にする", async () => {
    const response = await SELF.fetch("https://gallery.test/internal/photos", {
      method: "POST",
      headers: headers("wrong"),
      body: JPEG,
    });
    expect(response.status).toBe(401);
  });

  it("JPEG 以外を 415 にする", async () => {
    const badHeaders = new Headers(headers());
    badHeaders.set("Content-Type", "image/png");
    const response = await SELF.fetch("https://gallery.test/internal/photos", {
      method: "POST",
      headers: badHeaders,
      body: JPEG,
    });
    expect(response.status).toBe(415);
  });

  it("5 MB を超える写真を 413 にする", async () => {
    const oversized = new Uint8Array(5 * 1024 * 1024 + 1);
    oversized[0] = 0xff;
    oversized[1] = 0xd8;
    const response = await SELF.fetch("https://gallery.test/internal/photos", {
      method: "POST",
      headers: headers(),
      body: oversized,
    });
    expect(response.status).toBe(413);
  });

  it("不正な session_id を 400 にする", async () => {
    const response = await SELF.fetch("https://gallery.test/internal/photos", {
      method: "POST",
      headers: headers(KEY, "not-a-uuid"),
      body: JPEG,
    });
    expect(response.status).toBe(400);
  });
});

describe("GET /p/<token>", () => {
  it("写真ページと JPEG を配信しダウンロードヘッダを付ける", async () => {
    const { body } = await createPhoto();
    const page = await SELF.fetch(body.photo_url);
    expect(page.status).toBe(200);
    const html = await page.text();
    expect(html).toContain("この URL を知っている人は誰でも見られます。保存はお早めに");
    // ｽﾀｯｸﾁｬﾝ二次創作ガイドラインのクレジットとドット絵の出典
    expect(html).toContain("ｽﾀｯｸﾁｬﾝは");
    expect(html).toContain("https://github.com/stack-chan/stack-chan");
    expect(html).toContain("meganetaaan/mouse-follower");
    expect(page.headers.get("Cache-Control")).toBe("no-store");
    expect(page.headers.get("Referrer-Policy")).toBe("no-referrer");
    expect(page.headers.get("X-Robots-Tag")).toBe("noindex, nofollow");
    expect(page.headers.get("Content-Security-Policy")).toContain("default-src 'none'");
    // 保存ボタン: 写真アプリへ保存できるよう /save.js が画像を Web Share API で共有する
    expect(html).toContain(`href="/p/${body.token}.jpg?download=1"`);
    expect(html).toContain(`data-src="/p/${body.token}.jpg"`);
    expect(html).toMatch(/data-filename="stackchan-20261001T053000Z\.jpg"/);
    expect(html).toContain('<script src="/save.js"></script>');
    expect(page.headers.get("Content-Security-Policy")).toContain("script-src 'self'");
    expect(page.headers.get("Content-Security-Policy")).toContain("connect-src 'self'");

    const image = await SELF.fetch(`${body.photo_url}.jpg?download=1`);
    expect(image.status).toBe(200);
    expect(new Uint8Array(await image.arrayBuffer())).toEqual(JPEG);
    expect(image.headers.get("Content-Disposition")).toMatch(/^attachment; filename="stackchan-/);
  });

  it("保存ボタンのスクリプトを JavaScript として配信する", async () => {
    const script = await SELF.fetch("https://gallery.test/save.js");
    expect(script.status).toBe(200);
    expect(script.headers.get("Content-Type")).toBe("text/javascript; charset=utf-8");
    expect(script.headers.get("X-Robots-Tag")).toBe("noindex, nofollow");
    const source = await script.text();
    expect(source).toContain("navigator.share({ files: [file] })");
    expect(() => new Function(source)).not.toThrow();
  });

  it("GET 以外にも共通ヘッダを付けて 405 にする", async () => {
    const response = await SELF.fetch("https://gallery.test/p/AAAAAAAAAAAAAAAAAAAAAA", { method: "HEAD" });
    expect(response.status).toBe(405);
    expect(response.headers.get("Cache-Control")).toBe("no-store");
    expect(response.headers.get("X-Robots-Tag")).toBe("noindex, nofollow");
  });

  it("未知の token を 404 にする", async () => {
    expect((await SELF.fetch("https://gallery.test/p/AAAAAAAAAAAAAAAAAAAAAA")).status).toBe(404);
    expect((await SELF.fetch("https://gallery.test/p/AAAAAAAAAAAAAAAAAAAAAA.jpg")).status).toBe(404);
  });
});
