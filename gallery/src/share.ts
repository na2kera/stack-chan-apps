const X_INTENT = "https://x.com/intent/tweet?text=";

export function shareIntentUrl(text: string): string {
  return X_INTENT + encodeURIComponent(text);
}
