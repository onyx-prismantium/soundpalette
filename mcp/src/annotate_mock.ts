#!/usr/bin/env node
// Mock annotator (extension-4 §11): deterministic canned answers so the core's protocol
// handling is testable without any model. Answers come from a JSON map keyed by the file's
// basename (SP_MOCK_ANSWERS=path/to/answers.json); unknown files get a generic answer derived
// from the filename. SP_MOCK_MODE switches in one failure each:
//   normal            everything answers
//   malformed         stage-1 answer for the first request is a non-JSON line, then correct
//   unknown_category  category "NOT A CATEGORY" for every stage-1 answer
//   outside_shortlist stage-2 picks a valid CatID that is not among the candidates
//   error             every stage-1 answer is an {error}
//   timeout           the first describe request is never answered (later ones are)
//   crash             the process exits with code 3 on the first describe request
//   no_choose         hello advertises only the describe stage

import * as fs from "node:fs";
import * as path from "node:path";

import {
  ChooseRequest,
  DescribeRequest,
  HelloReply,
  PROTOCOL_VERSION,
  serve,
} from "./annotate_common.js";

const MODE = process.env.SP_MOCK_MODE ?? "normal";
const ANSWERS_FILE = process.env.SP_MOCK_ANSWERS ?? "";

type Canned = {
  description: string;
  fx_name: string;
  category: string;
  keywords: string[];
  confidence?: number;
  choose?: string; // CatID the mock "picks" in stage 2 (default: first candidate)
  choose_confidence?: number;
};

let answers: Record<string, Canned> = {};
if (ANSWERS_FILE) {
  answers = JSON.parse(fs.readFileSync(ANSWERS_FILE, "utf8")) as Record<string, Canned>;
}

let describeCount = 0;

function generic(req: DescribeRequest): Canned {
  const stem = path.basename(req.path).replace(/\.[^.]+$/, "");
  const words = stem.split(/[_\-\s.]+/).filter((w) => w.length > 0);
  return {
    description: `A ${words.join(" ").toLowerCase()} sound.`,
    fx_name: words.slice(0, 4).map((w) => w[0].toUpperCase() + w.slice(1).toLowerCase()).join(" "),
    category: req.categories[0]?.name ?? "AIR",
    keywords: words.map((w) => w.toLowerCase()),
    confidence: 0.5,
  };
}

const handler = {
  hello(): HelloReply {
    return {
      v: PROTOCOL_VERSION,
      hello: true,
      name: "soundpalette-annotate-mock",
      model: `mock-${MODE}`,
      prompt_version: "mock_v1",
      stages: MODE === "no_choose" ? ["describe"] : ["describe", "choose"],
    };
  },

  async describe(req: DescribeRequest): Promise<Record<string, unknown>> {
    const n = describeCount++;
    if (!fs.existsSync(req.audio.path)) throw new Error(`audio file missing: ${req.audio.path}`);
    const head = fs.readFileSync(req.audio.path).subarray(0, 4).toString("ascii");
    if (head !== "RIFF") throw new Error("audio is not a WAV file");
    if (MODE === "crash" && n === 0) process.exit(3);
    if (MODE === "timeout" && n === 0) {
      await new Promise(() => {}); // never resolves
    }
    if (MODE === "malformed" && n === 0) {
      process.stdout.write("this is not json\n");
      // then answer correctly below
    }
    if (MODE === "error") return { error: "mock refuses" };
    const canned = answers[path.basename(req.path)] ?? generic(req);
    const out: Record<string, unknown> = {
      description: canned.description,
      fx_name: canned.fx_name,
      category: MODE === "unknown_category" ? "NOT A CATEGORY" : canned.category,
      keywords: canned.keywords,
      confidence: canned.confidence ?? 0.5,
    };
    return out;
  },

  async choose(req: ChooseRequest): Promise<Record<string, unknown>> {
    const canned = answers[path.basename(req.path)];
    if (MODE === "outside_shortlist") {
      const inList = new Set(req.candidates.map((c) => c.cat_id));
      // a real CatID that is never a candidate for our fixtures
      const pick = inList.has("TOONBoing") ? "BELLLrg" : "TOONBoing";
      return { cat_id: pick, confidence: 0.9 };
    }
    const pick = canned?.choose ?? req.candidates[0]?.cat_id ?? "";
    return { cat_id: pick, confidence: canned?.choose_confidence ?? 0.8 };
  },
};

await serve(handler, 2);
