// Screenshots of the page against the harness (serve.py), with real phone emulation over CDP.
//   node tests/portal/page/shots.mjs <out dir> [base url] [chrome path]
// Each shot: [name, width, height, path+hash, optional JS run after load]. Needs node >= 22
// (built-in WebSocket) and a local Chrome; nothing is installed.
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const out = process.argv[2] || "shots";
const base = process.argv[3] || "http://127.0.0.1:8765";
const chrome = process.argv[4] || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const PHONE = [390, 844], DESK = [1280, 800];
const tap = (sel) => `document.querySelector(${JSON.stringify(sel)}).click()`;
const SHOTS = [
  ["healthy-drive", ...PHONE, "/?s=healthy#0"],
  ["problem-drive", ...PHONE, "/?s=problem#0"],
  ["problem-drive-full", ...PHONE, "/?s=problem#0", null, true],
  ["problem-battery", ...PHONE, "/?s=problem#1", null, true],
  ["healthy-battery", ...PHONE, "/?s=healthy#1", null, true],
  ["problem-bus", ...PHONE, "/?s=problem#2", null, true],
  ["problem-bus-expert", ...PHONE, "/?s=problem#2", tap("#xbtn"), true],
  ["problem-ecu", ...PHONE, "/?s=problem#2/ecus.b", null],
  ["healthy-ecu", ...PHONE, "/?s=healthy#2/ecus.a", null],
  ["problem-device", ...PHONE, "/?s=problem#3", `${tap("[data-g]")}`, true],
  ["healthy-device-en", ...PHONE, "/?s=healthy&lang=en#3", null, true],
  ["noui-healthy", ...PHONE, "/?s=healthy&ui=none#0", null, true],
  ["desk-problem-drive", ...DESK, "/?s=problem#0"],
  ["desk-healthy-battery", ...DESK, "/?s=healthy#1"],
  ["desk-problem-bus", ...DESK, "/?s=problem#2"],
];

const port = 9300 + Math.floor(Math.random() * 500);
const prof = mkdtempSync(join(tmpdir(), "portal-shots-"));
const proc = spawn(chrome, ["--headless=new", "--disable-gpu", "--hide-scrollbars", `--remote-debugging-port=${port}`,
  `--user-data-dir=${prof}`, "about:blank"], { stdio: "ignore" });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function target() {
  for (let i = 0; i < 50; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = list.find((t) => t.type === "page");
      if (page) return page.webSocketDebuggerUrl;
    } catch { /* not up yet */ }
    await sleep(200);
  }
  throw new Error("chrome did not start");
}

const ws = new WebSocket(await target());
await new Promise((r) => ws.addEventListener("open", r, { once: true }));
let id = 0;
const pending = new Map();
ws.addEventListener("message", (m) => {
  const msg = JSON.parse(m.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
});
const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evaluate = async (expr) => (await cdp("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;

mkdirSync(out, { recursive: true });
await cdp("Page.enable");
await cdp("Runtime.enable");
const report = [];
for (const [name, w, h, path, js, full] of SHOTS) {
  const mobile = w < 600;
  await cdp("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: mobile ? 2 : 1, mobile });
  await cdp("Page.navigate", { url: "about:blank" });
  await sleep(100);
  await cdp("Page.navigate", { url: base + path });
  await sleep(1800);
  if (js) { await evaluate(js); await sleep(400); }
  const m = await evaluate(`(() => { const d = document.documentElement, small = [...document.querySelectorAll("body *")]
      .filter(e => e.offsetParent && e.childNodes.length && [...e.childNodes].some(c => c.nodeType === 3 && c.textContent.trim()))
      .filter(e => parseFloat(getComputedStyle(e).fontSize) < 17).map(e => e.tagName + "." + e.className + ":" + e.textContent.trim().slice(0, 20));
    const tiny = [...document.querySelectorAll("a,button,select,input")].filter(e => e.offsetParent)
      .map(e => [e, e.getBoundingClientRect()]).filter(([e, r]) => r.height < 48 && !e.closest(".cr"))
      .map(([e, r]) => e.tagName + "." + e.className + ":" + Math.round(r.height) + ":" + e.textContent.trim().slice(0, 20));
    return { scrollW: d.scrollWidth, innerW: innerWidth, height: d.scrollHeight, small: small.slice(0, 8), tiny: tiny.slice(0, 8) }; })()`);
  let clip;
  if (full) clip = { x: 0, y: 0, width: w, height: Math.max(h, m.height), scale: 1 };
  const shot = await cdp("Page.captureScreenshot", { format: "png", captureBeyondViewport: !!full, ...(clip ? { clip } : {}) });
  writeFileSync(join(out, name + ".png"), Buffer.from(shot.result.data, "base64"));
  report.push({ name, ...m });
  await evaluate("localStorage.clear()");
}
console.log(JSON.stringify(report, null, 1));
ws.close();
proc.kill();
