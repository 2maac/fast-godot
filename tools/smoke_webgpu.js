#!/usr/bin/env node
// Headless smoke test for the WebGPU web export.
//
// Serves nothing itself: start a static server in the export directory first,
// e.g. `cd test_project/build-fast && python3 -m http.server 8125 --bind 127.0.0.1`.
//
// Usage:
//   NODE_PATH=/tmp/node-test/node_modules node tools/smoke_webgpu.js
//   PORT=8125 WAIT_MS=25000 SHOT=/tmp/grit.png node tools/smoke_webgpu.js
//
// Environment:
//   PORT       server port                 (default 8125)
//   CHROME     Chrome for Testing binary   (default /tmp/chrome-test/chrome-mac-arm64/...)
//   WAIT_MS    settle time after ready     (default 20000; the debug template
//              needs ~20 s of shader warm-up before it holds 60 FPS)
//   TIMEOUT_MS ready timeout               (default 150000)
//   SHOT       screenshot path             (default /tmp/webgpu_smoke.png)
//
// Requires `playwright-core` on NODE_PATH (the port's test harness lives in
// /tmp/node-test; `npm i playwright-core` anywhere works).

const { chromium } = require("playwright-core");
const fs = require("fs");

const PORT = process.env.PORT || "8125";
const CHROME =
  process.env.CHROME ||
  "/tmp/chrome-test/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing";
const WAIT_MS = Number(process.env.WAIT_MS || 20000);
const TIMEOUT_MS = Number(process.env.TIMEOUT_MS || 150000);
const SHOT = process.env.SHOT || "/tmp/webgpu_smoke.png";

(async () => {
  if (!fs.existsSync(CHROME)) {
    console.error(`Chrome for Testing not found at ${CHROME} (set CHROME=...)`);
    process.exit(2);
  }
  const browser = await chromium.launch({
    headless: true,
    executablePath: CHROME,
    args: ["--enable-unsafe-webgpu", "--no-first-run", "--no-default-browser-check"],
  });
  const page = await browser.newPage({ viewport: { width: 960, height: 540 } });

  const errors = [];
  page.on("console", (m) => {
    const text = m.text();
    if (m.type() === "error" || text.includes("GPUValidationError")) {
      errors.push(text);
      console.log("[console:error]", text.split("\n")[0]);
    }
  });
  page.on("pageerror", (e) => {
    errors.push("pageerror: " + e.message);
    console.log("[pageerror]", e.message);
  });

  await page.goto(`http://127.0.0.1:${PORT}/index.html`, { waitUntil: "load", timeout: 60000 });
  console.log("[probe] navigator.gpu =", await page.evaluate(() => !!navigator.gpu));

  let ready = false;
  try {
    await page.waitForFunction(() => window.__webgpu_test_ready === true, null, { timeout: TIMEOUT_MS });
    ready = true;
  } catch (e) {
    console.log("[wait] timed out:", e.message.split("\n")[0]);
  }
  if (ready) await page.waitForTimeout(WAIT_MS);

  const state = await page.evaluate(() => ({
    ready: window.__webgpu_test_ready === true,
    fps: window.__webgpu_fps || 0,
    draws: window.__webgpu_draws || 0,
    title: document.title,
    notice: (document.getElementById("status-notice") || {}).textContent || "",
    statusVisible: !!document.getElementById("status"),
  }));
  console.log("[state]", JSON.stringify(state));
  await page.screenshot({ path: SHOT });

  // Ignore the favicon 404 and the engine's benign shader-log warnings.
  const gpuErrors = errors.filter((e) => e.includes("GPUValidationError") || e.includes("WebGPU uncaptured error"));
  const otherErrors = errors.filter(
    (e) => !gpuErrors.includes(e) && !e.includes("404") && !e.includes("Compilation log") && !e.includes("code is unreachable"),
  );
  console.log(`[gpu-errors] ${gpuErrors.length} [other-errors] ${otherErrors.length} [screenshot] ${SHOT}`);

  const pass = ready && gpuErrors.length === 0 && otherErrors.length === 0 && state.fps >= 50;
  console.log(pass ? "RESULT: PASS" : "RESULT: FAIL");
  await browser.close();
  process.exit(pass ? 0 : 1);
})().catch((e) => {
  console.error("FATAL", e);
  process.exit(1);
});
