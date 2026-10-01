import { createExecutionContext, env, SELF, waitOnExecutionContext } from "cloudflare:test";
import { describe, expect, it, vi } from "vitest";

import worker from "../src/index";

const TOKEN = "AAAAAAAAAAAAAAAAAAAAAA";
const SESSION = "123e4567-e89b-42d3-a456-426614174000";
const EXPIRED = "2026-09-30T00:00:00.000Z";
const JPEG = new Uint8Array([0xff, 0xd8, 0xff, 0xd9]);

async function putExpired(): Promise<void> {
  await env.PHOTOS.put(`photos/${TOKEN}.jpg`, JPEG, {
    customMetadata: {
      session_id: SESSION,
      captured_at: "2026-09-29T23:00:00.000Z",
      expires_at: EXPIRED,
    },
    httpMetadata: { contentType: "image/jpeg" },
  });
  await env.PHOTOS.put(`sessions/${SESSION}`, TOKEN);
}

describe("expiry", () => {
  it("削除ジョブ前でも期限切れページと画像を 410 にする", async () => {
    await putExpired();
    const page = await SELF.fetch(`https://gallery.test/p/${TOKEN}`);
    expect(page.status).toBe(410);
    expect(await page.text()).toContain("この写真は削除されました");
    expect((await SELF.fetch(`https://gallery.test/p/${TOKEN}.jpg`)).status).toBe(410);
  });

  it("scheduled() が期限切れ写真と session 索引を削除する", async () => {
    await putExpired();
    const log = vi.spyOn(console, "log").mockImplementation(() => undefined);
    const ctx = createExecutionContext();
    await worker.scheduled!(
      { scheduledTime: Date.now(), cron: "*/5 * * * *", noRetry: () => undefined },
      env,
      ctx,
    );
    await waitOnExecutionContext(ctx);
    expect(await env.PHOTOS.get(`photos/${TOKEN}.jpg`)).toBeNull();
    expect(await env.PHOTOS.get(`sessions/${SESSION}`)).toBeNull();
    expect(log).toHaveBeenCalledWith(expect.stringContaining('"deleted":1'));
    log.mockRestore();
  });

  it("scheduled() は別の写真を指す session 索引を消さない", async () => {
    await putExpired();
    await env.PHOTOS.put(`sessions/${SESSION}`, "BBBBBBBBBBBBBBBBBBBBBB");
    const log = vi.spyOn(console, "log").mockImplementation(() => undefined);
    const ctx = createExecutionContext();
    await worker.scheduled!(
      { scheduledTime: Date.now(), cron: "*/5 * * * *", noRetry: () => undefined },
      env,
      ctx,
    );
    await waitOnExecutionContext(ctx);
    log.mockRestore();
    expect(await env.PHOTOS.get(`photos/${TOKEN}.jpg`)).toBeNull();
    expect(await (await env.PHOTOS.get(`sessions/${SESSION}`))!.text()).toBe("BBBBBBBBBBBBBBBBBBBBBB");
  });
});
