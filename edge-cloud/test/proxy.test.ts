import { describe, expect, it } from "vitest";
import { containerEnv, handle, MAX_FRAME_BYTES, type Forward, type ProxyEnv } from "../src/proxy";

const BASE = "https://edge.test";
const DEVICE_ID = "stackchan-01";
const KEY = "test-device-key";
const SESSION = "123e4567-e89b-42d3-a456-426614174000";
const FRAME_URL = `${BASE}/v1/sessions/${SESSION}/frames`;

const ENV: ProxyEnv = {
  DEVICE_ID,
  EDGE_DEVICE_KEY: KEY,
  GALLERY_URL: "https://gallery.test",
  GALLERY_KEY: "test-gallery-key",
};

const AUTH = { "X-Device-Id": DEVICE_ID, "X-Device-Key": KEY };

// forward に渡されたリクエストを記録し、コンテナの代わりに 200 を返す
function recorder() {
  const calls: Request[] = [];
  const forward: Forward = async (request) => {
    calls.push(request);
    return Response.json({ ready: true }, { status: 200 });
  };
  return { calls, forward };
}

function hello(headers: Record<string, string> = AUTH): Request {
  return new Request(`${BASE}/v1/hello`, {
    method: "POST",
    headers: { ...headers, "Content-Type": "application/json" },
    body: JSON.stringify({ device_id: DEVICE_ID, protocol_version: 1 }),
  });
}

async function expectError(response: Response, status: number, error: string) {
  expect(response.status).toBe(status);
  expect(await response.json()).toEqual({ error });
}

describe("ルーティング", () => {
  it.each(["/", "/v1", "/mock/p/abc", "/v2/hello", "/hello"])("%s は 404 で転送しない", async (path) => {
    const { calls, forward } = recorder();
    const response = await handle(new Request(`${BASE}${path}`, { headers: AUTH }), ENV, forward);
    await expectError(response, 404, "not_found");
    expect(calls).toHaveLength(0);
  });
});

describe("認証", () => {
  it.each([
    ["ヘッダなし", {}],
    ["鍵なし", { "X-Device-Id": DEVICE_ID }],
    ["ID なし", { "X-Device-Key": KEY }],
    ["鍵違い", { "X-Device-Id": DEVICE_ID, "X-Device-Key": "wrong" }],
    ["鍵の前方一致", { "X-Device-Id": DEVICE_ID, "X-Device-Key": KEY.slice(0, -1) }],
    ["ID 違い", { "X-Device-Id": "other", "X-Device-Key": KEY }],
    ["空の鍵", { "X-Device-Id": DEVICE_ID, "X-Device-Key": "" }],
  ])("%s は 401 で転送しない", async (_name, headers) => {
    const { calls, forward } = recorder();
    await expectError(await handle(hello(headers), ENV, forward), 401, "unauthorized");
    expect(calls).toHaveLength(0);
  });

  it.each([
    ["EDGE_DEVICE_KEY 未設定", { ...ENV, EDGE_DEVICE_KEY: undefined }],
    ["EDGE_DEVICE_KEY が空", { ...ENV, EDGE_DEVICE_KEY: "" }],
    ["DEVICE_ID 未設定", { ...ENV, DEVICE_ID: undefined }],
  ])("%s なら空の鍵でも正しい鍵でも 401", async (_name, env) => {
    const { calls, forward } = recorder();
    await expectError(await handle(hello(AUTH), env, forward), 401, "unauthorized");
    await expectError(
      await handle(hello({ "X-Device-Id": DEVICE_ID, "X-Device-Key": "" }), env, forward),
      401,
      "unauthorized",
    );
    expect(calls).toHaveLength(0);
  });

  it("認証前に gallery 設定の不備を明かさない", async () => {
    const { forward } = recorder();
    const env = { ...ENV, GALLERY_URL: "" };
    await expectError(await handle(hello({}), env, forward), 401, "unauthorized");
  });
});

describe("gallery の設定", () => {
  it.each([
    ["GALLERY_URL が空", { ...ENV, GALLERY_URL: "" }],
    ["GALLERY_URL 未設定", { ...ENV, GALLERY_URL: undefined }],
    ["GALLERY_URL が http", { ...ENV, GALLERY_URL: "http://gallery.test" }],
    ["GALLERY_KEY 未設定", { ...ENV, GALLERY_KEY: undefined }],
    ["GALLERY_KEY が空", { ...ENV, GALLERY_KEY: "" }],
  ])("%s なら 503 でコンテナを起こさない", async (_name, env) => {
    const { calls, forward } = recorder();
    await expectError(await handle(hello(), env, forward), 503, "misconfigured");
    expect(calls).toHaveLength(0);
  });
});

describe("フレームの大きさ", () => {
  it("Content-Length が 2 MiB を超えたら 413 で転送しない", async () => {
    const { calls, forward } = recorder();
    const request = new Request(FRAME_URL, {
      method: "POST",
      headers: { ...AUTH, "Content-Length": String(MAX_FRAME_BYTES + 1) },
    });
    await expectError(await handle(request, ENV, forward), 413, "frame_too_large");
    expect(calls).toHaveLength(0);
  });

  it("ちょうど 2 MiB は転送する (本文の検査は edge に任せる)", async () => {
    const { calls, forward } = recorder();
    const request = new Request(FRAME_URL, {
      method: "POST",
      headers: { ...AUTH, "Content-Length": String(MAX_FRAME_BYTES) },
    });
    expect((await handle(request, ENV, forward)).status).toBe(200);
    expect(calls).toHaveLength(1);
  });

  it("フレーム以外のパスでは Content-Length を見ない", async () => {
    const { calls, forward } = recorder();
    const request = new Request(`${BASE}/v1/sessions`, {
      method: "POST",
      headers: { ...AUTH, "Content-Length": String(MAX_FRAME_BYTES + 1) },
    });
    expect((await handle(request, ENV, forward)).status).toBe(200);
    expect(calls).toHaveLength(1);
  });
});

describe("転送", () => {
  it("認証を通ったリクエストをメソッド・パス・ヘッダ・本文そのままで渡す", async () => {
    const { calls, forward } = recorder();
    const body = new Uint8Array([0xff, 0xd8, 0xff, 0x00, 0x01, 0xff, 0xd9]);
    const request = new Request(`${FRAME_URL}?debug=1`, {
      method: "POST",
      headers: {
        ...AUTH,
        "Content-Type": "application/octet-stream",
        "X-Frame-Id": "42",
        "X-Format": "jpeg",
        "X-Phase": "capture",
      },
      body,
    });
    const response = await handle(request, ENV, forward);
    expect(response.status).toBe(200);
    expect(await response.json()).toEqual({ ready: true });
    expect(calls).toHaveLength(1);
    const forwarded = calls[0];
    expect(forwarded.method).toBe("POST");
    expect(new URL(forwarded.url).pathname).toBe(`/v1/sessions/${SESSION}/frames`);
    expect(new URL(forwarded.url).search).toBe("?debug=1");
    // コンテナ側の edge も同じ鍵で認証するので、認証ヘッダも消さずに渡す
    expect(forwarded.headers.get("X-Device-Id")).toBe(DEVICE_ID);
    expect(forwarded.headers.get("X-Device-Key")).toBe(KEY);
    expect(forwarded.headers.get("X-Frame-Id")).toBe("42");
    expect(forwarded.headers.get("Content-Type")).toBe("application/octet-stream");
    expect(new Uint8Array(await forwarded.arrayBuffer())).toEqual(body);
  });

  it("GET も転送し、コンテナの応答をそのまま返す", async () => {
    const forward: Forward = async () =>
      new Response(new Uint8Array([1, 2, 3]), { status: 404, headers: { "X-From": "edge" } });
    const request = new Request(`${BASE}/v1/sessions/${SESSION}/candidate`, { headers: AUTH });
    const response = await handle(request, ENV, forward);
    expect(response.status).toBe(404);
    expect(response.headers.get("X-From")).toBe("edge");
    expect(new Uint8Array(await response.arrayBuffer())).toEqual(new Uint8Array([1, 2, 3]));
  });
});

describe("containerEnv", () => {
  it("edge の環境変数名に詰め替える", () => {
    expect(containerEnv(ENV)).toEqual({
      EDGE_DEVICE_ID: DEVICE_ID,
      EDGE_DEVICE_KEY: KEY,
      GALLERY_URL: "https://gallery.test",
      GALLERY_KEY: "test-gallery-key",
    });
  });
});
