import { Container, getContainer } from "@cloudflare/containers";
import { containerEnv, serve, type ProxyEnv } from "./proxy";

export interface Env extends ProxyEnv {
  EDGE: DurableObjectNamespace<EdgeContainer>;
}

// edge (edge/Dockerfile) を 1 台だけ動かす。セッションは edge のメモリにあるので名前は固定。
const INSTANCE_NAME = "edge";

export class EdgeContainer extends Container<Env> {
  defaultPort = 8765;
  // device は app を開いている間 5 秒ごとに hello を送るので、使っている間は起きたまま。
  // 閉じてから 5 分で止める (止まっている間は課金されない)
  sleepAfter = "5m";
  // Worker の Secret / vars をコンテナの環境変数として渡す (イメージには入れない)。
  // フィールド初期化は super() の後に走るので this.env を読める
  envVars = containerEnv(this.env);
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    // 全応答で 1 件の構造化ログを出し、転送の例外は 502 upstream_error にする (serve())
    return serve(request, env, (req) => getContainer(env.EDGE, INSTANCE_NAME).fetch(req), {
      colo: typeof request.cf?.colo === "string" ? request.cf.colo : null,
    });
  },
} satisfies ExportedHandler<Env>;
