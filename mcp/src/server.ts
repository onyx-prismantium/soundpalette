// soundpalette-mcp: stdio MCP server exposing the SoundPalette analysis suite (extension §5.3).
// Thin wrapper: every tool shells out to the soundpalette CLI and returns its parsed --json
// output as structuredContent plus a short text rendering.
//
// Usage: node mcp/dist/server.js --root <project_dir> [--bin <path-to-soundpalette>]

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { Resvg } from "@resvg/resvg-js";
import { z } from "zod";

import { resolveExistingInRoot, resolveOutputInRoot, runCli, RootEscapeError } from "./cli.js";

function parseArgs(argv: string[]): { root: string; bin: string } {
  let root = "";
  let bin = "";
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--root" && i + 1 < argv.length) root = argv[++i];
    else if (argv[i] === "--bin" && i + 1 < argv.length) bin = argv[++i];
  }
  if (!root) {
    console.error("usage: server.js --root <project_dir> [--bin <path-to-soundpalette>]");
    process.exit(2);
  }
  root = fs.realpathSync(root);
  if (!bin) {
    // Default: build/cli/soundpalette relative to this repo checkout (extension §5.3).
    const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
    bin = path.join(repo, "build", "cli", "soundpalette");
    if (process.platform === "win32") bin += ".exe";
  }
  return { root, bin };
}

const { root: ROOT, bin: BIN } = parseArgs(process.argv.slice(2));

type ToolResult = {
  content: ({ type: "text"; text: string } | { type: "image"; data: string; mimeType: string })[];
  structuredContent?: Record<string, unknown>;
  isError?: boolean;
};

function toolError(message: string): ToolResult {
  return { content: [{ type: "text", text: `error: ${message}` }], isError: true };
}

/** Wraps a tool body so path-confinement and CLI failures surface as MCP tool errors. */
function guarded<A extends unknown[]>(fn: (...args: A) => Promise<ToolResult>) {
  return async (...args: A): Promise<ToolResult> => {
    try {
      return await fn(...args);
    } catch (e) {
      if (e instanceof RootEscapeError) return toolError(e.message);
      return toolError(e instanceof Error ? e.message : String(e));
    }
  };
}

const server = new McpServer({ name: "soundpalette-mcp", version: "0.2.0" });

server.registerTool(
  "scan_folder",
  {
    description:
      "Analyze every audio file under a folder (recursive) and return palette statistics. " +
      "Set include_files for the full per-file feature/visual list; out_path writes the " +
      "manifest JSON inside the project root.",
    inputSchema: {
      dir: z.string().describe("folder to scan, relative to the server root"),
      include_files: z.boolean().default(false),
      out_path: z.string().optional().describe("optional manifest output path inside root"),
    },
  },
  guarded(async ({ dir, include_files, out_path }) => {
    const dirAbs = resolveExistingInRoot(ROOT, dir);
    const res = await runCli(BIN, ["scan", dirAbs, "--quiet"]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `scan exited ${res.code}`);
    const manifest = JSON.parse(res.stdout);
    if (out_path !== undefined) {
      fs.writeFileSync(resolveOutputInRoot(ROOT, out_path), res.stdout);
    }
    const structured: Record<string, unknown> = {
      file_count: manifest.file_count,
      stats: manifest.stats,
    };
    if (include_files) structured.files = manifest.files;
    if (out_path !== undefined) structured.manifest_path = out_path;
    return {
      content: [
        {
          type: "text",
          text: `scanned ${manifest.file_count} files under ${dir}` +
            (out_path !== undefined ? `; manifest written to ${out_path}` : ""),
        },
      ],
      structuredContent: structured,
    };
  }),
);

server.registerTool(
  "lint_against_baseline",
  {
    description:
      "Scan a folder and flag files that drift off-palette relative to a baseline manifest's " +
      "stats (z-score over the seven mapping dimensions).",
    inputSchema: {
      dir: z.string(),
      baseline_path: z.string().optional(),
      profile_path: z.string().optional().describe("mutually exclusive with baseline_path"),
      threshold: z.number().default(2.5),
      top: z.number().int().default(10),
    },
  },
  guarded(async ({ dir, baseline_path, profile_path, threshold, top }) => {
    if ((baseline_path === undefined) === (profile_path === undefined)) {
      return toolError("provide exactly one of baseline_path or profile_path");
    }
    const dirAbs = resolveExistingInRoot(ROOT, dir);
    const refFlag = profile_path !== undefined ? "--profile" : "--baseline";
    const refAbs = resolveExistingInRoot(ROOT, (profile_path ?? baseline_path) as string);
    const res = await runCli(BIN, [
      "lint", dirAbs, refFlag, refAbs,
      "--threshold", String(threshold), "--top", String(top), "--json",
    ]);
    if (res.code === 2) return toolError(res.stderr.trim() || "lint failed");
    const report = JSON.parse(res.stdout);
    const lines: string[] = report.pass
      ? [`PASS: all files within the palette at threshold ${threshold}`]
      : report.outliers.map(
          (o: { path: string; category?: string; worst_dim: string; max_z: number;
                dims_over: string[] }) =>
            `OUTLIER ${o.path}${o.category ? ` cat=${o.category}` : ""} ` +
            `worst=${o.worst_dim} z=${o.max_z.toFixed(2)} dims=${o.dims_over.join(",")}`,
        );
    return { content: [{ type: "text", text: lines.join("\n") }], structuredContent: report };
  }),
);

server.registerTool(
  "describe_sound",
  {
    description:
      "Analyze one audio file and describe it in deterministic plain language, with the full " +
      "feature/visual/dimension breakdown as structured content.",
    inputSchema: { path: z.string() },
  },
  guarded(async ({ path: p }) => {
    const fileAbs = resolveExistingInRoot(ROOT, p);
    const res = await runCli(BIN, ["describe", fileAbs, "--json"]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `describe exited ${res.code}`);
    const detail = JSON.parse(res.stdout);
    return {
      content: [{ type: "text", text: detail.sentence }],
      structuredContent: detail,
    };
  }),
);

server.registerTool(
  "render_palette_sheet",
  {
    description:
      "Render the palette as a glyph-grid image (PNG). Pass either a folder to scan or an " +
      "existing manifest path.",
    inputSchema: {
      dir: z.string().optional(),
      manifest_path: z.string().optional(),
      columns: z.number().int().min(1).max(64).default(8),
      profile_path: z.string().optional().describe("draw deviation halos from this profile"),
    },
  },
  guarded(async ({ dir, manifest_path, columns, profile_path }) => {
    if ((dir === undefined) === (manifest_path === undefined)) {
      return toolError("provide exactly one of dir or manifest_path");
    }
    // Temp files stay inside root (extension §5.3: no writes outside --root, ever).
    const tmp = fs.mkdtempSync(path.join(ROOT, ".sp-mcp-"));
    try {
      let manifestAbs: string;
      let fileCount = 0;
      if (dir !== undefined) {
        const dirAbs = resolveExistingInRoot(ROOT, dir);
        manifestAbs = path.join(tmp, "palette.json");
        const scan = await runCli(BIN, ["scan", dirAbs, "--out", manifestAbs, "--quiet"]);
        if (scan.code !== 0) return toolError(scan.stderr.trim() || `scan exited ${scan.code}`);
      } else {
        manifestAbs = resolveExistingInRoot(ROOT, manifest_path as string);
      }
      fileCount = JSON.parse(fs.readFileSync(manifestAbs, "utf8")).file_count;
      const svgAbs = path.join(tmp, "sheet.svg");
      const svgArgs = ["export-svg", manifestAbs, "--out", svgAbs, "--columns", String(columns)];
      if (profile_path !== undefined) {
        svgArgs.push("--profile", resolveExistingInRoot(ROOT, profile_path));
      }
      const svg = await runCli(BIN, svgArgs);
      if (svg.code !== 0) return toolError(svg.stderr.trim() || `export-svg exited ${svg.code}`);
      const png = new Resvg(fs.readFileSync(svgAbs, "utf8")).render().asPng();
      return {
        content: [
          { type: "image", data: Buffer.from(png).toString("base64"), mimeType: "image/png" },
          { type: "text", text: `palette sheet: ${fileCount} sounds, ${columns} columns` },
        ],
      };
    } finally {
      fs.rmSync(tmp, { recursive: true, force: true });
    }
  }),
);

// ---- M10 profile tools (extension-2 §6.2); same root-confinement rules ----

// TS mirror of core's gating seam (extension-2 §6.3): the C++ capability() gates the CLI
// paths these tools shell into; this mirror marks the MCP-side seam sites. v3: fail-open.
function mcpCapability(feature: string): boolean {
  void feature;
  return true;
}

server.registerTool(
  "create_profile",
  {
    description:
      "Create a palette profile (.sppal.json) from a folder, optionally split into " +
      "per-category sub-profiles matched by path globs, or restricted to a curated selection.",
    inputSchema: {
      dir: z.string(),
      name: z.string(),
      categories: z.record(z.string(), z.array(z.string())).optional()
        .describe("category name -> glob patterns"),
      select_paths: z.array(z.string()).optional(),
      out_path: z.string(),
      threshold: z.number().default(2.5),
    },
  },
  guarded(async ({ dir, name, categories, select_paths, out_path, threshold }) => {
    if (!mcpCapability("mcp.write")) return toolError("mcp.write is not available");
    const dirAbs = resolveExistingInRoot(ROOT, dir);
    const outAbs = resolveOutputInRoot(ROOT, out_path);
    const args = ["profile", "create", dirAbs, "--name", name,
                  "--threshold", String(threshold), "--out", outAbs];
    for (const [catName, patterns] of Object.entries(categories ?? {})) {
      args.push("--category", `${catName}=${patterns.join(",")}`);
    }
    let selectFile: string | undefined;
    if (select_paths !== undefined) {
      selectFile = path.join(ROOT, `.sp-mcp-select-${process.pid}.txt`);
      fs.writeFileSync(selectFile, select_paths.join("\n") + "\n");
      args.push("--select", selectFile);
    }
    try {
      const res = await runCli(BIN, args);
      if (res.code !== 0) return toolError(res.stderr.trim() || `profile create exited ${res.code}`);
      const profile = JSON.parse(fs.readFileSync(outAbs, "utf8"));
      return {
        content: [
          {
            type: "text",
            text: `profile '${profile.name}': ${profile.created_from.file_count} files, ` +
              `${profile.categories.length} categories -> ${out_path}`,
          },
        ],
        structuredContent: {
          name: profile.name,
          file_count: profile.created_from.file_count,
          threshold: profile.threshold,
          categories: profile.categories.map(
            (c: { name: string; file_count: number }) => ({
              name: c.name, file_count: c.file_count,
            })),
          out_path,
        },
      };
    } finally {
      if (selectFile !== undefined) fs.rmSync(selectFile, { force: true });
    }
  }),
);

server.registerTool(
  "get_deviations",
  {
    description:
      "Full per-file deviation table for a folder against a profile: category, band, max_z " +
      "and the seven z-scores for every non-silent file.",
    inputSchema: {
      dir: z.string(),
      profile_path: z.string(),
      threshold: z.number().optional(),
    },
  },
  guarded(async ({ dir, profile_path, threshold }) => {
    const dirAbs = resolveExistingInRoot(ROOT, dir);
    const profAbs = resolveExistingInRoot(ROOT, profile_path);
    const args = ["lint", dirAbs, "--profile", profAbs, "--json", "--all", "--top", "10000"];
    if (threshold !== undefined) args.push("--threshold", String(threshold));
    const res = await runCli(BIN, args);
    if (res.code === 2) return toolError(res.stderr.trim() || "lint failed");
    const report = JSON.parse(res.stdout);
    const flagged = report.files.filter(
      (f: { band: string }) => f.band !== "none").length;
    return {
      content: [
        {
          type: "text",
          text: `${report.files.length} files, ${flagged} flagged ` +
            `(threshold ${report.threshold})`,
        },
      ],
      structuredContent: report,
    };
  }),
);

// ---- M9 harmonize tools (extension §6.5); same root-confinement rules ----

/** Resolves an output *directory* that may not exist yet: lexical containment first, then
 *  create, then realpath-verify (a symlink swapped in between would still be caught). */
function resolveOutputDirInRoot(root: string, p: string): string {
  const realRoot = fs.realpathSync(root);
  const candidate = path.resolve(realRoot, p);
  if (candidate !== realRoot && !candidate.startsWith(realRoot + path.sep)) {
    throw new RootEscapeError(p);
  }
  fs.mkdirSync(candidate, { recursive: true });
  const real = fs.realpathSync(candidate);
  if (real !== realRoot && !real.startsWith(realRoot + path.sep)) {
    throw new RootEscapeError(p);
  }
  return real;
}

server.registerTool(
  "propose_recipe",
  {
    description:
      "Propose a deterministic tier-one correction recipe (shelf EQ, attack soften, tail " +
      "shorten, loudness gain) pulling an off-palette sound back toward a baseline.",
    inputSchema: {
      path: z.string(),
      baseline_path: z.string(),
      threshold: z.number().default(2.5),
    },
  },
  guarded(async ({ path: p, baseline_path, threshold }) => {
    if (!mcpCapability("mcp.write")) return toolError("mcp.write is not available");
    const fileAbs = resolveExistingInRoot(ROOT, p);
    const baseAbs = resolveExistingInRoot(ROOT, baseline_path);
    const res = await runCli(BIN, [
      "propose", fileAbs, "--baseline", baseAbs, "--threshold", String(threshold),
    ]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `propose exited ${res.code}`);
    const recipe = JSON.parse(res.stdout);
    const r = recipe.result;
    return {
      content: [
        {
          type: "text",
          text:
            `proposed ${recipe.ops.length} op(s); max_z ${r.max_z_before} -> ${r.max_z_after}` +
            (r.unresolved.length ? `; unresolved: ${r.unresolved.join(", ")}` : ""),
        },
      ],
      structuredContent: recipe,
    };
  }),
);

server.registerTool(
  "apply_recipe",
  {
    description:
      "Apply a recipe to a source file (never modified) and write <stem>.harmonized.wav " +
      "into out_dir, with a post-analysis report.",
    inputSchema: {
      path: z.string(),
      recipe_path: z.string(),
      out_dir: z.string(),
    },
  },
  guarded(async ({ path: p, recipe_path, out_dir }) => {
    if (!mcpCapability("mcp.write")) return toolError("mcp.write is not available");
    const fileAbs = resolveExistingInRoot(ROOT, p);
    const recipeAbs = resolveExistingInRoot(ROOT, recipe_path);
    const outDirAbs = resolveOutputDirInRoot(ROOT, out_dir);
    const stem = path.basename(fileAbs).replace(/\.[^.]+$/, "");
    const wavOut = path.join(outDirAbs, `${stem}.harmonized.wav`);
    const reportOut = path.join(outDirAbs, `${stem}.harmonized.report.json`);
    const res = await runCli(BIN, [
      "apply", fileAbs, "--recipe", recipeAbs, "--out", wavOut, "--report", reportOut,
    ]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `apply exited ${res.code}`);
    const report = JSON.parse(fs.readFileSync(reportOut, "utf8"));
    return {
      content: [{ type: "text", text: `wrote ${path.relative(ROOT, wavOut)}` }],
      structuredContent: { output_path: path.relative(ROOT, wavOut), report },
    };
  }),
);

server.registerTool(
  "harmonize",
  {
    description:
      "Harmonize every off-palette file in a folder (or one file) against a baseline: " +
      "propose + apply + sidecar recipes, sources never modified. dry_run writes recipes only.",
    inputSchema: {
      path_or_dir: z.string(),
      baseline_path: z.string(),
      out_dir: z.string().default("harmonized"),
      threshold: z.number().default(2.5),
      dry_run: z.boolean().default(false),
    },
  },
  guarded(async ({ path_or_dir, baseline_path, out_dir, threshold, dry_run }) => {
    if (!dry_run && !mcpCapability("mcp.write")) return toolError("mcp.write is not available");
    const targetAbs = resolveExistingInRoot(ROOT, path_or_dir);
    const baseAbs = resolveExistingInRoot(ROOT, baseline_path);
    const outDirAbs = resolveOutputDirInRoot(ROOT, out_dir);
    const args = [
      "harmonize", targetAbs, "--baseline", baseAbs, "--out-dir", outDirAbs,
      "--threshold", String(threshold),
    ];
    if (dry_run) args.push("--dry-run");
    const res = await runCli(BIN, args);
    if (res.code === 2) return toolError(res.stderr.trim() || "harmonize failed");
    const results = res.stdout
      .trim()
      .split(/\r?\n/)
      .filter((l) => l.length > 0)
      .map((line) => {
        const harmonized = line.match(/^HARMONIZED (.+) max_z ([\d.]+) -> ([\d.]+)$/);
        if (harmonized) {
          return {
            path: harmonized[1],
            status: "harmonized",
            max_z_before: Number(harmonized[2]),
            max_z_after: Number(harmonized[3]),
          };
        }
        const unresolved = line.match(/^UNRESOLVED (.+) dims=(.*)$/);
        if (unresolved) {
          return {
            path: unresolved[1],
            status: "unresolved",
            dims: unresolved[2] ? unresolved[2].split(",") : [],
          };
        }
        return { status: "unknown", line };
      });
    return {
      content: [{ type: "text", text: res.stdout.trim() || "nothing to harmonize" }],
      structuredContent: { all_within_threshold: res.code === 0, results },
    };
  }),
);

// ---------------------------------------------------------------------------------------------
// Extension-4 §9.3: the sound library. Every tool shells out to `soundpalette library ... --json`
// and confines paths to --root like everything else.

function libraryArgs(dir: string): string {
  return resolveExistingInRoot(ROOT, dir);
}

server.registerTool(
  "library_search",
  {
    description:
      "Search a library index (created with `soundpalette library init`) by free text over " +
      "names, descriptions and keywords, with optional UCS category / CatID filters. Returns " +
      "rows with path, CatID, FX name, description, confidence and provenance.",
    inputSchema: {
      dir: z.string().default(".").describe("library root or any path inside it, relative to the server root"),
      query: z.string().default(""),
      category: z.string().optional().describe("exact UCS category, e.g. AMBIENCE"),
      cat_id: z.string().optional().describe("CatID glob, e.g. AMB* or GUNPis"),
      unannotated: z.boolean().default(false),
      min_confidence: z.number().optional(),
      limit: z.number().int().default(100),
    },
  },
  guarded(async ({ dir, query, category, cat_id, unannotated, min_confidence, limit }) => {
    const args = ["library", "search", libraryArgs(dir), "--json", "--limit", String(limit)];
    if (query) args.push("--query", query);
    if (category) args.push("--category", category);
    if (cat_id) args.push("--catid", cat_id);
    if (unannotated) args.push("--unannotated");
    if (min_confidence !== undefined) args.push("--min-confidence", String(min_confidence));
    const res = await runCli(BIN, args);
    if (res.code !== 0) return toolError(res.stderr.trim() || `library search exited ${res.code}`);
    const data = JSON.parse(res.stdout);
    const lines = (data.results as { path: string; annotation: { cat_id?: string; fx_name?: string; description?: string } | null }[])
      .map((r) => `${r.annotation?.cat_id || "-"}\t${r.path}\t${r.annotation?.fx_name ?? ""}: ${r.annotation?.description ?? ""}`);
    return {
      content: [{ type: "text", text: lines.length ? lines.join("\n") : "no results" }],
      structuredContent: { count: data.count, results: data.results },
    };
  }),
);

server.registerTool(
  "library_get",
  {
    description: "One library row: analysis entry plus its UCS annotation and provenance.",
    inputSchema: { path: z.string().describe("audio file inside a library, relative to the server root") },
  },
  guarded(async ({ path: p }) => {
    const res = await runCli(BIN, ["library", "show", resolveExistingInRoot(ROOT, p), "--json"]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `library show exited ${res.code}`);
    const data = JSON.parse(res.stdout);
    const a = data.annotation;
    return {
      content: [{ type: "text", text: a ? `${a.cat_id || "-"} ${a.fx_name}: ${a.description} [${a.source}${a.locked ? ", locked" : ""}]` : "unannotated" }],
      structuredContent: data,
    };
  }),
);

server.registerTool(
  "library_set_annotation",
  {
    description:
      "Human edit of a library row (locks it against model overwrites). Empty fields are left " +
      "unchanged; cat_id must be a UCS CatID.",
    inputSchema: {
      path: z.string(),
      cat_id: z.string().optional(),
      fx_name: z.string().optional(),
      description: z.string().optional(),
      keywords: z.array(z.string()).optional(),
    },
  },
  guarded(async ({ path: p, cat_id, fx_name, description, keywords }) => {
    if (!mcpCapability("mcp.write")) return toolError("mcp.write is not available");
    const args = ["library", "set", resolveExistingInRoot(ROOT, p)];
    if (cat_id !== undefined) args.push("--catid", cat_id);
    if (fx_name !== undefined) args.push("--fx-name", fx_name);
    if (description !== undefined) args.push("--description", description);
    if (keywords !== undefined) args.push("--keywords", keywords.join(","));
    if (args.length === 3) return toolError("nothing to set");
    const res = await runCli(BIN, args);
    if (res.code !== 0) return toolError(res.stderr.trim() || `library set exited ${res.code}`);
    return { content: [{ type: "text", text: res.stdout.trim() }] };
  }),
);

server.registerTool(
  "library_annotate",
  {
    description:
      "Run the model annotator over library rows (unannotated by default, or the given paths). " +
      "Long-running; returns the summary and per-file results. Uses the server's SP_ANNOTATOR " +
      "or the annotator argument.",
    inputSchema: {
      dir: z.string().default("."),
      paths: z.array(z.string()).optional().describe("specific files instead of the folder selection"),
      all: z.boolean().default(false),
      dry_run: z.boolean().default(false),
      limit: z.number().int().optional(),
      annotator: z.string().optional().describe("annotator command override"),
      timeout_s: z.number().default(120),
    },
  },
  guarded(async ({ dir, paths, all, dry_run, limit, annotator, timeout_s }) => {
    if (!dry_run && !mcpCapability("mcp.write")) return toolError("mcp.write is not available");
    const common: string[] = ["--json", "--timeout", String(timeout_s)];
    if (dry_run) common.push("--dry-run");
    if (annotator) common.push("--annotator", annotator);
    const reports: Record<string, unknown>[] = [];
    if (paths && paths.length > 0) {
      for (const p of paths) {
        const res = await runCli(BIN, ["library", "annotate", resolveExistingInRoot(ROOT, p), ...common]);
        if (res.code !== 0 && !res.stdout.trim()) return toolError(res.stderr.trim() || "annotate failed");
        reports.push(JSON.parse(res.stdout));
      }
    } else {
      const args = ["library", "annotate", libraryArgs(dir), ...common];
      if (all) args.push("--all");
      if (limit !== undefined) args.push("--limit", String(limit));
      const res = await runCli(BIN, args);
      if (res.code !== 0 && !res.stdout.trim()) return toolError(res.stderr.trim() || "annotate failed");
      reports.push(JSON.parse(res.stdout));
    }
    const sum = (k: string) => reports.reduce((n, r) => n + (Number(r[k]) || 0), 0);
    const files = reports.flatMap((r) => (r.files as unknown[]) ?? []);
    const fatal = reports.map((r) => String(r.fatal ?? "")).filter((f) => f).join("; ");
    const text = `annotated ${sum("annotated")} of ${sum("requested")} (${sum("errors")} errors, ` +
      `${sum("skipped_locked")} locked)` + (fatal ? `; fatal: ${fatal}` : "");
    return {
      content: [{ type: "text", text }],
      structuredContent: {
        requested: sum("requested"), annotated: sum("annotated"), errors: sum("errors"),
        skipped_locked: sum("skipped_locked"), timeouts: sum("timeouts"), fatal, files,
        annotator: reports[0]?.annotator,
      },
      isError: fatal.length > 0,
    };
  }),
);

server.registerTool(
  "library_export_manifest",
  {
    description:
      "Write a canonical manifest (scan-compatible JSON) of the library rows matching a search, " +
      "so lint_against_baseline / render_palette_sheet / propose_recipe can consume a search result.",
    inputSchema: {
      dir: z.string().default("."),
      out_path: z.string().describe("manifest output path inside the server root"),
      query: z.string().default(""),
      category: z.string().optional(),
      cat_id: z.string().optional(),
      unannotated: z.boolean().default(false),
    },
  },
  guarded(async ({ dir, out_path, query, category, cat_id, unannotated }) => {
    const outAbs = resolveOutputInRoot(ROOT, out_path);
    const args = ["library", "export", libraryArgs(dir), "--manifest", outAbs];
    if (query) args.push("--query", query);
    if (category) args.push("--category", category);
    if (cat_id) args.push("--catid", cat_id);
    if (unannotated) args.push("--unannotated");
    const res = await runCli(BIN, args);
    if (res.code !== 0) return toolError(res.stderr.trim() || `library export exited ${res.code}`);
    const manifest = JSON.parse(fs.readFileSync(outAbs, "utf8"));
    return {
      content: [{ type: "text", text: `${res.stdout.trim()}` }],
      structuredContent: { manifest_path: out_path, file_count: manifest.file_count, stats: manifest.stats },
    };
  }),
);

await server.connect(new StdioServerTransport());
