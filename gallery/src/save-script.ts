// 写真ページの「保存」ボタン用スクリプト。/save.js で配る (CSP は script-src 'self')。
// 対応ブラウザでは画像ファイルを Web Share API で共有し、iPhone なら共有シートの「画像を保存」で
// 写真アプリに入れられるようにする。共有できないときはリンク本来のダウンロードのまま。
// iOS Safari はクリックから share() までに await を挟むと拒否されるので、画像はページを開いた時点で
// 取得しておき、クリックでは同期的に share() を呼ぶ。
export const SAVE_SCRIPT = `(() => {
  const btn = document.getElementById("save");
  if (!btn || !navigator.canShare || !navigator.share || typeof File !== "function") return;
  let file = null;
  fetch(btn.dataset.src, { credentials: "same-origin" })
    .then((r) => (r.ok ? r.blob() : Promise.reject(new Error(String(r.status)))))
    .then((blob) => {
      const f = new File([blob], btn.dataset.filename, { type: "image/jpeg" });
      if (navigator.canShare({ files: [f] })) file = f;
    })
    .catch(() => {});
  btn.addEventListener("click", (e) => {
    if (!file) return;
    e.preventDefault();
    navigator.share({ files: [file] }).catch((err) => {
      if (err && err.name === "AbortError") return;
      location.href = btn.href;
    });
  });
})();
`;
