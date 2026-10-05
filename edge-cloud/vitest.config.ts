import { defineConfig } from "vitest/config";

// テストするのは src/proxy.ts (認証・ルーティング) だけなので素の vitest (Node) で回す。
// コンテナ本体 (src/index.ts) は Docker とデプロイが要るので、ここでは試さない
// (受け入れチェックは docs/design/step5-edge-cloud.md)。
export default defineConfig({
  test: {
    include: ["test/**/*.test.ts"],
  },
});
