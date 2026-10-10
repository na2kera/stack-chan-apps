import { describe, expect, it } from "vitest";
import {
  containerEnv,
  handle,
  MAX_FRAME_BYTES,
  requestKind,
  serve,
  type Forward,
  type ProxyEnv,
} from "../src/proxy";

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

function frame(headers: Record<string, string>): Request {
  return new Request(FRAME_URL, { method: "POST", headers: { ...AUTH, ...headers } });
}

describe("フレームの Content-Length (edge と同じ契約)", () => {
  it("無ければ 411 length_required で転送しない", async () => {
    const { calls, forward } = recorder();
    // 本文がストリームだと Content-Length が付かない (chunked と同じ)
    const request = new Request(FRAME_URL, {
      method: "POST",
      headers: AUTH,
      body: new ReadableStream({
        start(controller) {
          controller.enqueue(new Uint8Array([1, 2, 3]));
          controller.close();
        },
      }),
      duplex: "half",
    } as RequestInit);
    expect(request.headers.get("Content-Length")).toBeNull();
    await expectError(await handle(request, ENV, forward), 411, "length_required");
    expect(calls).toHaveLength(0);
  });

  it.each(["abc", "-1", "1.5", "1e6", "0x10", ""])(
    "%j は 400 invalid_header:content-length で転送しない",
    async (value) => {
      const { calls, forward } = recorder();
      await expectError(
        await handle(frame({ "Content-Length": value }), ENV, forward),
        400,
        "invalid_header:content-length",
      );
      expect(calls).toHaveLength(0);
    },
  );

  it("認証より前には判定しない (鍵なしは 401)", async () => {
    const { forward } = recorder();
    const request = new Request(FRAME_URL, { method: "POST" });
    await expectError(await handle(request, ENV, forward), 401, "unauthorized");
  });

  it("frames 以外の POST (本文なし) は Content-Length が無くても転送する", async () => {
    const { calls, forward } = recorder();
    const request = new Request(`${BASE}/v1/sessions/${SESSION}/timeout`, {
      method: "POST",
      headers: AUTH,
    });
    expect((await handle(request, ENV, forward)).status).toBe(200);
    expect(calls).toHaveLength(1);
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
        "Content-Length": String(body.length),
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

describe("アクセスログ (serve)", () => {
  function capture() {
    const lines: string[] = [];
    let t = 1000;
    return {
      lines,
      options: {
        colo: "NRT",
        log: (line: string) => lines.push(line),
        now: () => {
          t += 7;
          return t;
        },
      },
    };
  }

  it.each([
    ["404", new Request(`${BASE}/`), 404, "other"],
    ["401", hello({}), 401, "hello"],
    ["411", new Request(FRAME_URL, { method: "POST", headers: AUTH }), 411, "frame"],
    ["413", frame({ "Content-Length": String(MAX_FRAME_BYTES + 1) }), 413, "frame"],
  ])("早期の %s でちょうど 1 件 (event early)", async (_name, request, status, kind) => {
    const { calls, forward } = recorder();
    const { lines, options } = capture();
    const response = await serve(request as Request, ENV, forward, options);
    expect(response.status).toBe(status);
    expect(calls).toHaveLength(0);
    expect(lines).toHaveLength(1);
    expect(JSON.parse(lines[0])).toEqual({ event: "early", kind, status, ms: 7, colo: "NRT" });
  });

  it("503 misconfigured も 1 件だけ", async () => {
    const { forward } = recorder();
    const { lines, options } = capture();
    const response = await serve(hello(), { ...ENV, GALLERY_URL: "" }, forward, options);
    expect(response.status).toBe(503);
    expect(lines).toHaveLength(1);
    expect(JSON.parse(lines[0])).toMatchObject({ event: "early", status: 503 });
  });

  it("転送の成功はコンテナの status で 1 件 (event forward)", async () => {
    const forward: Forward = async () => new Response("no", { status: 404 });
    const { lines, options } = capture();
    const request = new Request(`${BASE}/v1/sessions/${SESSION}/candidate`, { headers: AUTH });
    expect((await serve(request, ENV, forward, options)).status).toBe(404);
    expect(lines).toHaveLength(1);
    expect(JSON.parse(lines[0])).toEqual({
      event: "forward",
      kind: "candidate",
      status: 404,
      ms: 7,
      colo: "NRT",
    });
  });

  it("転送の例外は 502 upstream_error で 1 件 (event forward_error)", async () => {
    const forward: Forward = async () => {
      throw new Error(`connect failed ${FRAME_URL} key=${KEY}`);
    };
    const { lines, options } = capture();
    const response = await serve(hello(), ENV, forward, options);
    await expectError(response, 502, "upstream_error");
    expect(lines).toHaveLength(1);
    expect(JSON.parse(lines[0])).toMatchObject({ event: "forward_error", kind: "hello", status: 502 });
  });

  it("colo が無ければ null", async () => {
    const { forward } = recorder();
    const lines: string[] = [];
    await serve(hello(), ENV, forward, { log: (line) => lines.push(line) });
    expect(JSON.parse(lines[0]).colo).toBeNull();
  });

  it("ログに鍵・ID・URL・セッション ID・ヘッダ値・本文を含まない", async () => {
    const lines: string[] = [];
    const log = (line: string) => lines.push(line);
    const body = new Uint8Array([0xff, 0xd8, 0x42, 0x42, 0xff, 0xd9]);
    const requests = [
      hello(),
      hello({ "X-Device-Id": DEVICE_ID, "X-Device-Key": "wrong-secret-value" }),
      new Request(`${FRAME_URL}?token=abc`, {
        method: "POST",
        headers: { ...AUTH, "Content-Length": String(body.length), "X-Frame-Id": "4242" },
        body,
      }),
    ];
    const forward: Forward = async () => {
      throw new Error(`boom ${KEY}`);
    };
    for (const request of requests) await serve(request, ENV, forward, { log, colo: "NRT" });
    expect(lines).toHaveLength(3);
    const text = lines.join("\n");
    for (const secret of [KEY, "wrong-secret-value", DEVICE_ID, SESSION, "edge.test", "token", "4242", "boom"]) {
      expect(text).not.toContain(secret);
    }
    for (const line of lines) {
      expect(Object.keys(JSON.parse(line)).sort()).toEqual(["colo", "event", "kind", "ms", "status"]);
    }
  });
});

describe("requestKind", () => {
  it.each([
    ["POST", "/v1/hello", "hello"],
    ["POST", `/v1/sessions/${SESSION}/frames`, "frame"],
    ["GET", `/v1/sessions/${SESSION}/candidate`, "candidate"],
    ["POST", `/v1/sessions/${SESSION}/review`, "save"],
    ["GET", `/v1/sessions/${SESSION}/photo`, "photo"],
    ["POST", "/v1/sessions", "other"],
    ["POST", `/v1/sessions/${SESSION}/timeout`, "other"],
    ["GET", "/v1/hello", "other"],
  ])("%s %s → %s", (method, path, kind) => {
    expect(requestKind(method, path)).toBe(kind);
  });
});
