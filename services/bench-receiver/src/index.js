// The start-up benchmark receiver: POST /reports stores one benchmark report
// per series in D1 with its day of receipt and nothing else
// (docs/spec/benchmark-v1.md, section 9). A daily Cron Trigger deletes reports
// past the retention period.

import { checkReport } from "./report.js";
import { deleteExpired } from "./retention.js";

export default {
  async fetch(request, env) {
    const outcome = await receive(request, env);
    // One line per request: status, format and size; never the IP, the body
    // or the series id.
    console.log(JSON.stringify({ status: outcome.status, format: outcome.format ?? null, bytes: outcome.bytes ?? null }));
    const headers = { "content-type": "application/json; charset=utf-8" };
    if (outcome.status === 405) headers.allow = "POST";
    return new Response(JSON.stringify({ message: outcome.message }), { status: outcome.status, headers });
  },

  async scheduled(controller, env) {
    const deleted = await deleteExpired(env, new Date(controller.scheduledTime));
    console.log(JSON.stringify({ retention: deleted }));
  },
};

function config(env) {
  return {
    maxBodyBytes: Number(env.MAX_BODY_BYTES),
    dailyLimit: Number(env.DAILY_LIMIT),
    dailyBytesLimit: Number(env.DAILY_BYTES_LIMIT),
    retentionMonths: Number(env.RETENTION_MONTHS),
    minStartMs: Number(env.MIN_START_MS),
    maxStartMs: Number(env.MAX_START_MS),
  };
}

async function receive(request, env) {
  const url = new URL(request.url);
  if (url.pathname !== "/reports") return { status: 404, message: "Not found." };
  if (request.method !== "POST") return { status: 405, message: "Use POST." };
  const limits = config(env);

  // 1. Size and type.
  const declared = request.headers.get("content-length");
  if (declared !== null && Number(declared) > limits.maxBodyBytes) {
    return { status: 413, message: `The report is larger than ${limits.maxBodyBytes} bytes.` };
  }
  if (!isJson(request.headers.get("content-type"))) {
    return { status: 415, message: "Send the report as application/json." };
  }
  const bytes = await readLimited(request, limits.maxBodyBytes);
  if (bytes === null) return { status: 413, message: `The report is larger than ${limits.maxBodyBytes} bytes.` };
  const size = bytes.byteLength;

  // 2. Per IP. The address stays in the rate limiter's counters only.
  const { success } = await env.RATE_LIMITER.limit({ key: request.headers.get("cf-connecting-ip") ?? "unknown" });
  if (!success) return { status: 429, message: "Too many reports from this address. Try again later.", bytes: size };

  const day = new Date().toISOString().slice(0, 10);
  try {
    // 3. Across all senders, per UTC day.
    const today = await env.DB.prepare("SELECT received, bytes FROM daily_counts WHERE day = ?1").bind(day).first();
    if ((today?.received ?? 0) >= limits.dailyLimit || (today?.bytes ?? 0) >= limits.dailyBytesLimit) {
      return { status: 503, message: "The receiver has taken all the reports it can today. Try again tomorrow.", bytes: size };
    }

    // 4. Validation.
    let body, report;
    try {
      body = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true }).decode(bytes);
      report = JSON.parse(body);
    } catch {
      return { status: 422, message: "The report is not UTF-8 JSON.", bytes: size };
    }
    const checked = checkReport(report, limits);
    if (!checked.ok) return { status: 422, message: checked.message, format: checked.format, bytes: size };

    // 5. Store the body byte for byte, once per series.
    const insert = await env.DB.prepare(
      "INSERT OR IGNORE INTO reports (series_id, format, received_day, body) VALUES (?1, ?2, ?3, ?4)",
    )
      .bind(report.series_id, checked.format, day, body)
      .run();
    if (insert.meta.changes === 0) {
      return { status: 200, message: "This report has already arrived.", format: checked.format, bytes: size };
    }
    await env.DB.prepare(
      "INSERT INTO daily_counts (day, received, bytes) VALUES (?1, 1, ?2) " +
        "ON CONFLICT (day) DO UPDATE SET received = received + 1, bytes = bytes + ?2",
    )
      .bind(day, size)
      .run();
    return { status: 201, message: "Received. Thank you.", format: checked.format, bytes: size };
  } catch {
    // D1 unavailable or over its daily limits.
    return { status: 503, message: "The receiver cannot store reports right now. Try again later.", bytes: size };
  }
}

function isJson(contentType) {
  if (!contentType) return false;
  const [type, ...params] = contentType.split(";").map((part) => part.trim().toLowerCase());
  if (type !== "application/json") return false;
  return params.every((p) => !p.startsWith("charset=") || p === "charset=utf-8" || p === 'charset="utf-8"');
}

// Reads the body up to `max` bytes; null when it is longer.
async function readLimited(request, max) {
  if (!request.body) return new Uint8Array(0);
  const reader = request.body.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > max) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return bytes;
}
