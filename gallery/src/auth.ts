const encoder = new TextEncoder();

async function digest(value: string): Promise<Uint8Array> {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", encoder.encode(value)));
}

export async function verifyGalleryKey(
  provided: string | null,
  expected: string | undefined,
): Promise<boolean> {
  // Secret 未設定や空のヘッダでは必ず拒否する (空同士のダイジェストが一致してしまうため)。
  if (!expected || !provided) return false;
  const [actualDigest, expectedDigest] = await Promise.all([
    digest(provided),
    digest(expected),
  ]);
  let difference = 0;
  for (let i = 0; i < expectedDigest.length; i += 1) {
    difference |= actualDigest[i] ^ expectedDigest[i];
  }
  return difference === 0;
}
