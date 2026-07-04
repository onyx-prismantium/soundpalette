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

// ---- M9 harmonize tools (extension §6.5), rooted at fixtures_m9 ----

const M9_ROOT = path.join(REPO, "fixtures_m9");
const M9_BASELINE = "m9-baseline.json";
let m9: Client;

before(async () => {
  assert.ok(fs.existsSync(path.join(M9_ROOT, "fixable_outlier.wav")),
    "missing fixtures_m9 (run genfixtures --extra first)");
  await execFileP(BIN, ["scan", ROOT, "--out", path.join(M9_ROOT, M9_BASELINE), "--quiet"]);
  m9 = new Client({ name: "mcp-smoke-m9", version: "0.0.0" });
  await m9.connect(new StdioClientTransport({
    command: process.execPath,
    args: [SERVER, "--root", M9_ROOT, "--bin", BIN],
  }));
});

after(async () => {
  await m9?.close();
  fs.rmSync(path.join(M9_ROOT, M9_BASELINE), { force: true });
  fs.rmSync(path.join(M9_ROOT, "out"), { recursive: true, force: true });
});

test("propose_recipe returns a deterministic recipe with ops", async () => {
  const call = () => m9.callTool({
    name: "propose_recipe",
    arguments: { path: "fixable_outlier.wav", baseline_path: M9_BASELINE },
  }) as Promise<{ structuredContent?: { ops?: unknown[]; result?: { max_z_before: number } } }>;
  const a = await call();
  const b = await call();
  assert.ok((a.structuredContent?.ops?.length ?? 0) >= 1);
  assert.deepEqual(a.structuredContent, b.structuredContent); // §6.4: pure
});

test("harmonize closes the loop and reports per-file results", async () => {
  const res = await m9.callTool({
    name: "harmonize",
    arguments: {
      path_or_dir: "fixable_outlier.wav",
      baseline_path: M9_BASELINE,
      out_dir: "out",
    },
  }) as {
    structuredContent?: {
      all_within_threshold?: boolean;
      results?: { path: string; status: string }[];
    };
  };
  assert.equal(res.structuredContent?.all_within_threshold, true);
  assert.equal(res.structuredContent?.results?.[0]?.status, "harmonized");
  assert.ok(fs.existsSync(path.join(M9_ROOT, "out", "fixable_outlier.harmonized.wav")));
  assert.ok(fs.existsSync(
    path.join(M9_ROOT, "out", "fixable_outlier.harmonized.recipe.json")));
});

test("apply_recipe writes output + report inside out_dir", async () => {
  const res = await m9.callTool({
    name: "apply_recipe",
    arguments: {
      path: "fixable_outlier.wav",
      recipe_path: "out/fixable_outlier.harmonized.recipe.json",
      out_dir: "out/applied",
    },
  }) as { structuredContent?: { output_path?: string; report?: { post?: unknown } } };
  assert.ok(res.structuredContent?.output_path?.endsWith("fixable_outlier.harmonized.wav"));
  assert.ok(res.structuredContent?.report?.post);
});

test("harmonize out_dir cannot escape the root", async () => {
  const res = await m9.callTool({
    name: "harmonize",
    arguments: {
      path_or_dir: "fixable_outlier.wav",
      baseline_path: M9_BASELINE,
      out_dir: "../escape-attempt",
    },
  }) as { isError?: boolean };
  assert.equal(res.isError, true);
});

// ---- M10 profile tools (extension-2 §6.2), rooted at fixtures_m10 ----

const M10_ROOT = path.join(REPO, "fixtures_m10");
let m10: Client;

before(async () => {
  assert.ok(fs.existsSync(path.join(M10_ROOT, "catfx", "ui", "ui_00.wav")),
    "missing fixtures_m10 (run genfixtures --profile-set first)");
  m10 = new Client({ name: "mcp-smoke-m10", version: "0.0.0" });
  await m10.connect(new StdioClientTransport({
    command: process.execPath,
    args: [SERVER, "--root", M10_ROOT, "--bin", BIN],
  }));
  // A misplaced ui tick inside combat/ for the deviation-table tests.
  fs.copyFileSync(path.join(M10_ROOT, "catfx", "ui", "ui_00.wav"),
    path.join(M10_ROOT, "catfx", "combat", "misplaced.wav"));
});

after(async () => {
  await m10?.close();
  fs.rmSync(path.join(M10_ROOT, "catfx", "combat", "misplaced.wav"), { force: true });
  fs.rmSync(path.join(M10_ROOT, "catfx.sppal.json"), { force: true });
});

test("create_profile roundtrips against profile show", async () => {
  const res = await m10.callTool({
    name: "create_profile",
    arguments: {
      dir: "catfx",
      name: "catfx",
      categories: { ui: ["ui/**", "**/ui_*"], combat: ["combat/**"] },
      out_path: "catfx.sppal.json",
    },
  }) as {
    structuredContent?: {
      name?: string; categories?: { name: string; file_count: number }[];
    };
  };
  assert.equal(res.structuredContent?.name, "catfx");
  const cats = res.structuredContent?.categories ?? [];
  assert.equal(cats.find((c) => c.name === "ui")?.file_count, 8);
  // combat now holds the misplaced tick too (9 files) — count comes from the profile itself.
  assert.ok((cats.find((c) => c.name === "combat")?.file_count ?? 0) >= 8);

  const show = await execFileP(BIN, ["profile", "show",
    path.join(M10_ROOT, "catfx.sppal.json")]);
  assert.match(show.stdout, /profile catfx/);
});

test("get_deviations returns the full 17-entry table with the misplaced tick red", async () => {
  const res = await m10.callTool({
    name: "get_deviations",
    arguments: { dir: "catfx", profile_path: "catfx.sppal.json" },
  }) as {
    structuredContent?: {
      files?: { path: string; band: string; category: string; max_z: number }[];
    };
  };
  const files = res.structuredContent?.files ?? [];
  assert.equal(files.length, 17);
  const misplaced = files.find((f) => f.path === "combat/misplaced.wav");
  assert.ok(misplaced, "misplaced.wav missing from the table");
  assert.equal(misplaced?.category, "combat");
  // The profile was created with the misplaced tick already inside combat (9 files); one
  // foreign tick cannot drag the 8 genuine hits' stats far enough to hide itself.
  assert.equal(misplaced?.band, "red");
});

test("render_palette_sheet with profile_path returns an image", async () => {
  const res = await m10.callTool({
    name: "render_palette_sheet",
    arguments: { dir: "catfx", columns: 6, profile_path: "catfx.sppal.json" },
  }) as { isError?: boolean; content: { type: string; mimeType?: string; data?: string }[] };
  assert.notEqual(res.isError, true);
  const image = res.content.find((c) => c.type === "image");
  assert.equal(image?.mimeType, "image/png");
  assert.ok((image?.data ?? "").length > 10000);
});
