// SDK-client integration tests for soundpalette-mcp (extension §7 mcp_smoke.sh suite).
// Spawns the built server over stdio against the generated v1 fixture set.

import assert from "node:assert/strict";
import { after, before, test } from "node:test";
import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const execFileP = promisify(execFile);

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const ROOT = path.join(REPO, "tests", "golden", "fixtures");
const BIN = path.join(REPO, "build", "cli",
  process.platform === "win32" ? "soundpalette.exe" : "soundpalette");
const SERVER = path.join(REPO, "mcp", "dist", "server.js");
const BASELINE = "mcp-test-baseline.json"; // inside ROOT, removed in after()

let client: Client;

before(async () => {
  assert.ok(fs.existsSync(BIN), `missing CLI binary: ${BIN} (build first)`);
  assert.ok(fs.existsSync(path.join(ROOT, "sine440_1s.wav")),
    "missing fixtures (run genfixtures first)");
  await execFileP(BIN, ["scan", path.join(ROOT, "darkset"),
    "--out", path.join(ROOT, BASELINE), "--quiet"]);

  client = new Client({ name: "mcp-smoke", version: "0.0.0" });
  await client.connect(new StdioClientTransport({
    command: process.execPath,
    args: [SERVER, "--root", ROOT, "--bin", BIN],
  }));
});

after(async () => {
  await client?.close();
  fs.rmSync(path.join(ROOT, BASELINE), { force: true });
});

test("tools/list contains the four M8 tools", async () => {
  const { tools } = await client.listTools();
  const names = tools.map((t) => t.name).sort();
  for (const expected of
    ["describe_sound", "lint_against_baseline", "render_palette_sheet", "scan_folder"]) {
    assert.ok(names.includes(expected), `missing tool ${expected} in [${names.join(", ")}]`);
  }
});

test("describe_sound: sine440 is very tonal, structured content has flatness", async () => {
  const t0 = Date.now();
  const res = await client.callTool({
    name: "describe_sound",
    arguments: { path: "sine440_1s.wav" },
  }) as { content: { type: string; text?: string }[]; structuredContent?: Record<string, unknown> };
  const elapsed = Date.now() - t0;
  assert.ok(elapsed < 2000, `describe_sound took ${elapsed} ms (budget 2 s)`);

  const text = res.content.find((c) => c.type === "text")?.text ?? "";
  assert.match(text, /very tonal/);
  assert.match(JSON.stringify(res.structuredContent), /"flatness"/);
});

test("lint_against_baseline flags bright_outlier against the darkset baseline", async () => {
  const res = await client.callTool({
    name: "lint_against_baseline",
    arguments: { dir: ".", baseline_path: BASELINE, threshold: 4.0, top: 50 },
  }) as { content: { type: string; text?: string }[]; structuredContent?: { pass: boolean } };
  const text = res.content.find((c) => c.type === "text")?.text ?? "";
  assert.match(text, /bright_outlier\.wav/);
  assert.equal(res.structuredContent?.pass, false);
});

test("render_palette_sheet returns a PNG image", async () => {
  const t0 = Date.now();
  const res = await client.callTool({
    name: "render_palette_sheet",
    arguments: { dir: "darkset", columns: 8 },
  }) as { content: { type: string; data?: string; mimeType?: string }[] };
  const elapsed = Date.now() - t0;
  assert.ok(elapsed < 5000, `render_palette_sheet took ${elapsed} ms (budget 5 s)`);

  const image = res.content.find((c) => c.type === "image");
  assert.ok(image, "no image content returned");
  assert.equal(image?.mimeType, "image/png");
  assert.ok((image?.data ?? "").length > 10000,
    `base64 image too small: ${(image?.data ?? "").length}`);
});

test("scan_folder honors include_files and stays summary-only by default", async () => {
  const brief = await client.callTool({
    name: "scan_folder",
    arguments: { dir: "darkset" },
  }) as { structuredContent?: Record<string, unknown> };
  assert.equal(brief.structuredContent?.file_count, 20);
  assert.equal(brief.structuredContent?.files, undefined);

  const full = await client.callTool({
    name: "scan_folder",
    arguments: { dir: "darkset", include_files: true },
  }) as { structuredContent?: { files?: unknown[] } };
  assert.equal(full.structuredContent?.files?.length, 20);
});

test("path traversal outside --root is rejected", async () => {
  const res = await client.callTool({
    name: "describe_sound",
    arguments: { path: "../../../../../../etc/passwd" },
  }) as { isError?: boolean; content: { type: string; text?: string }[] };
  assert.equal(res.isError, true);
  assert.match(res.content[0]?.text ?? "", /error/i);
});
