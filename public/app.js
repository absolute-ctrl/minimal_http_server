// Polls the server's /api/stats endpoint and shows the result.
const out = document.getElementById("stats");

async function refresh() {
  try {
    const res = await fetch("/api/stats", { cache: "no-store" });
    out.textContent = JSON.stringify(await res.json(), null, 2);
  } catch (err) {
    out.textContent = "server unreachable: " + err;
  }
}

refresh();
setInterval(refresh, 2000);
