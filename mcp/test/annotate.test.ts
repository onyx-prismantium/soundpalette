// Unit tests for the annotator's protocol helpers and a stdio round trip with the mock
// (extension-4 §11). No model, no network.

import assert from "node:assert/strict";
import { test } from "node:test";
import { spawn } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

import {
  DescribeAnswerSchema,
  extractJson,
  normalizeDescribe,
} from "../dist/annotate_common.js";

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const MOCK = path.join(REPO, "mcp", "dist", "annotate_mock.js");

test("extractJson tolerates fences, prose and trailing chatter", () => {
  assert.deepEqual(extractJson('{"a": 1}'), { a: 1 });
  assert.deepEqual(extractJson('Sure! ```json\n{"a": "x}y", "b": [1,2]}\n``` hope this helps'),
    { a: "x}y", b: [1, 2] });
  assert.deepEqual(extractJson('prefix {"nested": {"k": "v"}} suffix {"second": 2}'),
    { nested: { k: "v" } });
  assert.equal(extractJson("no json here"), null);
  assert.equal(extractJson('{"broken": '), null);
});

test("normalizeDescribe caps and lowercases", () => {
  const a = normalizeDescribe(DescribeAnswerSchema.parse({
    description: "  A   heavy   door  creaks.  ",
    fx_name: "one two three four five six seven eight",
    category: "doors",
    keywords: ["Door", "door", "CREAK", " wood "],
  }));
  assert.equal(a.description, "A heavy door creaks.");
  assert.equal(a.fx_name, "one two three four five six");
  assert.equal(a.category, "DOORS");
  assert.deepEqual(a.keywords, ["door", "creak", "wood"]);
  assert.equal(a.confidence, 0.5);
});

test("mock annotator round trip over stdio", async () => {
  assert.ok(fs.existsSync(MOCK), "build mcp first");
  const wav = path.join(os.tmpdir(), `sp-annotate-test-${process.pid}.wav`);
  fs.writeFileSync(wav, Buffer.concat([Buffer.from("RIFF"), Buffer.alloc(40)]));
  const child = spawn(process.execPath, [MOCK], { stdio: ["pipe", "pipe", "inherit"] });
  const lines: string[] = [];
  let buf = "";
  child.stdout.on("data", (d: Buffer) => {
    buf += d.toString();
    let nl;
    while ((nl = buf.indexOf("\n")) >= 0) {
      lines.push(buf.slice(0, nl));
      buf = buf.slice(nl + 1);
    }
  });
  const send = (o: unknown) => child.stdin.write(JSON.stringify(o) + "\n");
  send({ v: 1, hello: true });
  send({
    v: 1, id: "7", stage: "describe", path: "x/thunder_rumble.wav",
    audio: { path: wav, sample_rate: 16000, duration_s: 1.0 },
    categories: [{ name: "WEATHER", sub_categories: ["THUNDER"] }],
  });
  send({
    v: 1, id: "8", stage: "choose", path: "x/thunder_rumble.wav", description: "d",
    candidates: [{ cat_id: "THUN", category: "WEATHER", sub_category: "THUNDER" }],
  });
  send({ v: 1, id: "9", stage: "bogus" });
  child.stdin.end();
  await new Promise<void>((resolve) => child.on("close", () => resolve()));
  fs.rmSync(wav, { force: true });
  const msgs = lines.map((l) => JSON.parse(l) as Record<string, unknown>);
  const hello = msgs.find((m) => m.hello === true);
  assert.ok(hello, "hello reply");
  assert.deepEqual(hello!.stages, ["describe", "choose"]);
  const d = msgs.find((m) => m.id === "7")!;
  assert.equal(d.category, "WEATHER");
  assert.equal(d.fx_name, "Thunder Rumble");
  assert.deepEqual(d.keywords, ["thunder", "rumble"]);
  const c = msgs.find((m) => m.id === "8")!;
  assert.equal(c.cat_id, "THUN");
  const e = msgs.find((m) => m.id === "9")!;
  assert.match(String(e.error), /unknown stage/);
});
