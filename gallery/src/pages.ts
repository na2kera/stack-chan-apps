import type { PhotoMetadata } from "./store";

const STYLE = `
body{font-family:system-ui,-apple-system,"Hiragino Sans",sans-serif;margin:0 auto;padding:16px;
background:#fafafa;color:#222;max-width:320px}
img{width:100%;height:auto;border-radius:8px;background:#ddd}
.btn{display:block;text-align:center;padding:14px;margin:16px 0;border-radius:8px;
background:#1d9bf0;color:#fff;text-decoration:none;font-weight:bold}
.note{font-size:.9em;color:#555}
`;

function page(title: string, body: string): string {
  return `<!doctype html><html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex,nofollow"><meta name="referrer" content="no-referrer"><title>${title}</title><style>${STYLE}</style></head><body>${body}</body></html>`;
}

export function formatJst(iso: string): string {
  const parts = new Intl.DateTimeFormat("ja-JP", {
    timeZone: "Asia/Tokyo",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
    hourCycle: "h23",
  }).formatToParts(new Date(iso));
  const value = (type: Intl.DateTimeFormatPartTypes) =>
    parts.find((part) => part.type === type)?.value ?? "";
  return `${value("year")}-${value("month")}-${value("day")} ${value("hour")}:${value("minute")}`;
}

export function renderPhotoPage(token: string, metadata: PhotoMetadata): string {
  const image = `/p/${token}.jpg`;
  return page(
    "スタックチャンの写真",
    `<h1>スタックチャンの写真</h1><img src="${image}" alt="写真"><a class="btn" href="${image}?download=1">写真をダウンロード</a><p>削除予定: ${formatJst(metadata.expires_at)}（日本時間）</p><p class="note">この URL を知っている人は誰でも見られます。保存はお早めに</p>`,
  );
}

export function renderExpiredPage(): string {
  return page("写真の期限切れ", "<h1>この写真は削除されました</h1>");
}

export function renderNotFoundPage(): string {
  return page("写真が見つかりません", "<h1>写真が見つかりません</h1>");
}
