import { cloudflareTest } from "@cloudflare/vitest-pool-workers";
import { defineConfig } from "vitest/config";

// @cloudflare/vitest-pool-workers 0.22 (vitest 4 系) は plugin 形式。
// wrangler.jsonc の R2 バインディングと vars を miniflare に読み込み、Secret だけここで与える。
export default defineConfig({
  plugins: [
    cloudflareTest({
      wrangler: { configPath: "./wrangler.jsonc" },
      miniflare: {
        bindings: { GALLERY_KEY: "test-gallery-key" },
      },
    }),
  ],
});
