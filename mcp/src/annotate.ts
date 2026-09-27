#!/usr/bin/env node
// Reference annotator (extension-4 §7.3): speaks the JSON-lines protocol on stdio and calls
// any OpenAI-compatible chat-completions endpoint that accepts `input_audio` content parts —
// llama.cpp `llama-server` or vLLM serving Qwen2-Audio locally, or a hosted API.
//
// Configuration (env, or flags of the same name without SP_LLM_ and lowercased):
//   SP_LLM_BASE_URL     default http://127.0.0.1:8080/v1
//   SP_LLM_API_KEY      optional bearer token
//   SP_LLM_MODEL        model id sent in the request (default: the endpoint's first model)
//   SP_LLM_CONCURRENCY  parallel requests (default 2)
//   SP_LLM_MAX_TOKENS   default 400
//   SP_LLM_TEMPERATURE  default 0.2
// Never logs audio or keys. Diagnostics go to stderr; stdout is protocol only.

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

import {
  ChooseAnswerSchema,
  ChooseRequest,
  DescribeAnswerSchema,
  DescribeRequest,
  extractJson,
  HelloReply,
  normalizeDescribe,
  parsePrompt,
  PromptParts,
  PROTOCOL_VERSION,
  serve,
} from "./annotate_common.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PROMPT_DIR = path.resolve(HERE, "..", "prompts");
const PROMPT_VERSION = "annotate_v1";

function env(name: string, fallback: string): string {
  const flag = "--" + name.replace(/^SP_LLM_/, "").toLowerCase().replace(/_/g, "-");
  const i = process.argv.indexOf(flag);
  if (i >= 0 && i + 1 < process.argv.length) return process.argv[i + 1];
  return process.env[name] ?? fallback;
}

const BASE_URL = env("SP_LLM_BASE_URL", "http://127.0.0.1:8080/v1").replace(/\/+$/, "");
const API_KEY = env("SP_LLM_API_KEY", "");
let MODEL = env("SP_LLM_MODEL", "");
const CONCURRENCY = Math.max(1, parseInt(env("SP_LLM_CONCURRENCY", "2"), 10) || 2);
const MAX_TOKENS = parseInt(env("SP_LLM_MAX_TOKENS", "400"), 10) || 400;
const TEMPERATURE = parseFloat(env("SP_LLM_TEMPERATURE", "0.2"));

function loadPrompt(): PromptParts {
  const file = path.join(PROMPT_DIR, PROMPT_VERSION + ".md");
  try {
    return parsePrompt(fs.readFileSync(file, "utf8"));
  } catch (e) {
    throw new Error(`${file}: ${(e as Error).message}`);
  }
}

const prompt = loadPrompt();

type Message = { role: "system" | "user" | "assistant"; content: unknown };

async function chat(messages: Message[]): Promise<string> {
  const headers: Record<string, string> = { "content-type": "application/json" };
  if (API_KEY) headers["authorization"] = `Bearer ${API_KEY}`;
  const body: Record<string, unknown> = {
    model: MODEL,
    messages,
    max_tokens: MAX_TOKENS,
    temperature: TEMPERATURE,
  };
  const res = await fetch(`${BASE_URL}/chat/completions`, {
    method: "POST",
    headers,
    body: JSON.stringify(body),
  });
  if (!res.ok) {
    const detail = (await res.text()).slice(0, 300);
    throw new Error(`endpoint ${res.status}: ${detail}`);
  }
  const data = (await res.json()) as {
    choices?: { message?: { content?: unknown } }[];
    model?: string;
  };
  const content = data.choices?.[0]?.message?.content;
  if (typeof content === "string") return content;
  if (Array.isArray(content)) {
    return content
      .map((p) => (typeof p === "string" ? p : (p as { text?: string }).text ?? ""))
      .join("");
  }
  throw new Error("endpoint returned no message content");
}

async function resolveModel(): Promise<string> {
  if (MODEL) return MODEL;
  try {
    const headers: Record<string, string> = {};
    if (API_KEY) headers["authorization"] = `Bearer ${API_KEY}`;
    const res = await fetch(`${BASE_URL}/models`, { headers });
    if (res.ok) {
      // OpenAI shape {data:[{id}]}; llama.cpp also answers {models:[{name}]}
      const data = (await res.json()) as { data?: { id?: string }[]; models?: { name?: string }[] };
      const id = data.data?.[0]?.id ?? data.models?.[0]?.name;
      if (id) return id;
    }
  } catch {
    // fall through
  }
  return "default";
}

function fillTemplate(t: string, vars: Record<string, string>): string {
  return t.replace(/\{\{(\w+)\}\}/g, (_, k: string) => vars[k] ?? "");
}

function categoriesText(req: DescribeRequest): string {
  return req.categories
    .map((c) => (c.sub_categories?.length ? `${c.name}: ${c.sub_categories.join(", ")}` : c.name))
    .join("\n");
}

function hintsText(req: DescribeRequest): string {
  const a = req.analysis;
  if (!a) return "(none)";
  const parts: string[] = [];
  if (typeof a.duration_s === "number") parts.push(`duration ${a.duration_s.toFixed(2)} s`);
  if (typeof a.lufs_i === "number" && a.lufs_i > -200) parts.push(`${a.lufs_i.toFixed(1)} LUFS`);
  if (a.silent) parts.push("measured as silent");
  if (a.describe) parts.push(`analysis words: ${a.describe}`);
  if (a.psycho) {
    const p = a.psycho;
    if (typeof p.sones_n5 === "number") parts.push(`loudness ${p.sones_n5.toFixed(1)} sone`);
    if (typeof p.sharpness_acum === "number") parts.push(`sharpness ${p.sharpness_acum.toFixed(2)} acum`);
    if (typeof p.roughness_asper === "number") parts.push(`roughness ${p.roughness_asper.toFixed(2)} asper`);
  }
  if (req.audio.truncated) parts.push("only the first 30 s are attached");
  return parts.join("; ") || "(none)";
}

async function askJson<T>(
  messages: Message[],
  parse: (raw: unknown) => T,
): Promise<T> {
  let text = await chat(messages);
  for (let attempt = 0; attempt < 2; attempt++) {
    const raw = extractJson(text);
    if (raw !== null) {
      try {
        return parse(raw);
      } catch (e) {
        if (attempt === 1) throw new Error(`model answer failed validation: ${(e as Error).message}`);
      }
    } else if (attempt === 1) {
      throw new Error("model did not return JSON");
    }
    text = await chat([
      ...messages,
      { role: "assistant", content: text },
      { role: "user", content: prompt.retry },
    ]);
  }
  throw new Error("unreachable");
}

const handler = {
  hello(): HelloReply {
    return {
      v: PROTOCOL_VERSION,
      hello: true,
      name: "soundpalette-annotate",
      model: MODEL,
      prompt_version: PROMPT_VERSION,
      stages: ["describe", "choose"],
    };
  },

  async describe(req: DescribeRequest): Promise<Record<string, unknown>> {
    const wav = fs.readFileSync(req.audio.path);
    const userText = fillTemplate(prompt.describe, {
      categories: categoriesText(req),
      hints: hintsText(req),
      filename: path.basename(req.path),
    });
    const messages: Message[] = [
      { role: "system", content: prompt.system },
      {
        role: "user",
        content: [
          { type: "input_audio", input_audio: { data: wav.toString("base64"), format: "wav" } },
          { type: "text", text: userText },
        ],
      },
    ];
    const answer = await askJson(messages, (raw) => normalizeDescribe(DescribeAnswerSchema.parse(raw)));
    return { ...answer };
  },

  async choose(req: ChooseRequest): Promise<Record<string, unknown>> {
    const candidates = req.candidates
      .map((c) => `- ${c.cat_id} (${c.category} / ${c.sub_category}): ${c.explanation ?? ""}`)
      .join("\n");
    const userText = fillTemplate(prompt.choose, {
      description: req.description,
      fx_name: req.fx_name ?? "",
      keywords: (req.keywords ?? []).join(", "),
      candidates,
    });
    const messages: Message[] = [
      { role: "system", content: prompt.system },
      { role: "user", content: userText },
    ];
    const answer = await askJson(messages, (raw) => ChooseAnswerSchema.parse(raw));
    return { cat_id: answer.cat_id.trim(), confidence: answer.confidence };
  },
};

MODEL = await resolveModel();
process.stderr.write(`soundpalette-annotate: ${BASE_URL} model=${MODEL} prompt=${PROMPT_VERSION}\n`);
await serve(handler, CONCURRENCY);
