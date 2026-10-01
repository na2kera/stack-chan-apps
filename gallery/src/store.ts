import { createToken } from "./token";

export interface Env {
  PHOTOS: R2Bucket;
  GALLERY_KEY?: string;
  TTL_MINUTES: string;
  SHARE_TEXT: string;
  PUBLIC_BASE_URL: string;
}

export interface PhotoMetadata {
  session_id: string;
  captured_at: string;
  expires_at: string;
}

export interface StoredPhoto {
  token: string;
  object: R2ObjectBody;
  metadata: PhotoMetadata;
}

export interface PutPhotoResult extends StoredPhoto {
  created: boolean;
}

function photoKey(token: string): string {
  return `photos/${token}.jpg`;
}

function sessionKey(sessionId: string): string {
  return `sessions/${sessionId}`;
}

export function isExpired(metadata: PhotoMetadata, now = new Date()): boolean {
  return Date.parse(metadata.expires_at) <= now.getTime();
}

export async function getPhoto(bucket: R2Bucket, token: string): Promise<StoredPhoto | null> {
  const object = await bucket.get(photoKey(token));
  if (object === null) return null;
  const metadata = object.customMetadata as unknown as Partial<PhotoMetadata>;
  if (!metadata.session_id || !metadata.captured_at || !metadata.expires_at) return null;
  return { token, object, metadata: metadata as PhotoMetadata };
}

export async function findBySession(
  bucket: R2Bucket,
  sessionId: string,
  now = new Date(),
): Promise<StoredPhoto | null> {
  const session = await bucket.get(sessionKey(sessionId));
  if (session === null) return null;
  const token = await session.text();
  const photo = await getPhoto(bucket, token);
  return photo !== null && !isExpired(photo.metadata, now) ? photo : null;
}

export async function putPhoto(
  bucket: R2Bucket,
  sessionId: string,
  jpeg: ArrayBuffer,
  capturedAt: string,
  ttlMinutes: number,
  now = new Date(),
): Promise<PutPhotoResult> {
  const index = await bucket.get(sessionKey(sessionId));
  if (index !== null) {
    const current = await getPhoto(bucket, await index.text());
    if (current !== null && !isExpired(current.metadata, now)) return { ...current, created: false };
  }

  const token = createToken();
  const metadata: PhotoMetadata = {
    session_id: sessionId,
    captured_at: capturedAt,
    expires_at: new Date(now.getTime() + ttlMinutes * 60_000).toISOString(),
  };
  // 写真 → 索引の順に書く。索引は条件付き (無ければ作る / 読んだ版のままなら置き換える) にして、
  // 同じ session_id の並行アップロードで勝つのは 1 件だけにする。負けた側は自分の写真を消して勝者を返す。
  await bucket.put(photoKey(token), jpeg, {
    customMetadata: { ...metadata },
    httpMetadata: { contentType: "image/jpeg" },
  });
  const onlyIf = index === null ? new Headers({ "If-None-Match": "*" }) : { etagMatches: index.etag };
  const written = await bucket.put(sessionKey(sessionId), token, { onlyIf });
  if (written === null) {
    await bucket.delete(photoKey(token));
    const winner = await findBySession(bucket, sessionId, now);
    if (winner === null) throw new Error("session index conflict");
    return { ...winner, created: false };
  }
  const object = await bucket.get(photoKey(token));
  if (object === null) throw new Error("photo disappeared after upload");
  return { token, object, metadata, created: true };
}

async function deleteInChunks(bucket: R2Bucket, keys: string[]): Promise<void> {
  for (let index = 0; index < keys.length; index += 1000) {
    await bucket.delete(keys.slice(index, index + 1000));
  }
}

export async function deleteExpired(bucket: R2Bucket, now = new Date()): Promise<number> {
  let cursor: string | undefined;
  let removed = 0;
  do {
    const page = await bucket.list({ prefix: "photos/", cursor, include: ["customMetadata"] });
    const photoKeys: string[] = [];
    const sessionKeys: string[] = [];
    for (const object of page.objects) {
      const metadata = object.customMetadata as unknown as Partial<PhotoMetadata>;
      if (metadata.expires_at && Date.parse(metadata.expires_at) <= now.getTime()) {
        photoKeys.push(object.key);
        // 索引がこの写真を指しているときだけ消す (同じ session の新しい写真の索引を巻き込まない)。
        if (metadata.session_id) {
          const key = sessionKey(metadata.session_id);
          const index = await bucket.get(key);
          const token = object.key.slice("photos/".length, -".jpg".length);
          if (index !== null && (await index.text()) === token) sessionKeys.push(key);
        }
      }
    }
    await deleteInChunks(bucket, photoKeys);
    await deleteInChunks(bucket, sessionKeys);
    removed += photoKeys.length;
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor !== undefined);
  return removed;
}
