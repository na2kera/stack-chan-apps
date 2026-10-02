// テストで import する `cloudflare:test` の env の型 (wrangler.jsonc のバインディングと Secret)。
declare namespace Cloudflare {
  interface Env {
    PHOTOS: R2Bucket;
    GALLERY_KEY: string;
    TTL_MINUTES: string;
    SHARE_TEXT: string;
    PUBLIC_BASE_URL: string;
  }
}
