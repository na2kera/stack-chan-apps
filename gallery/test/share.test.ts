import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";

describe("GET /share/x", () => {
  it("設定文だけを X intent に渡して 302 にする", async () => {
    const response = await SELF.fetch("https://gallery.test/share/x", { redirect: "manual" });
    expect(response.status).toBe(302);
    const location = response.headers.get("Location") ?? "";
    const redirect = new URL(location);
    expect(`${redirect.origin}${redirect.pathname}`).toBe("https://x.com/intent/tweet");
    expect(redirect.searchParams.get("text")).toBe(
      "@na2kera_0510 の #ｽﾀｯｸﾁｬﾝ に写真を撮ってもらいました！　#StackChan #n_study",
    );
    expect(location).not.toContain("gallery.test");
    expect(location).not.toContain("/p/");
  });
});
