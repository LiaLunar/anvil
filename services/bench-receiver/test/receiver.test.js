import { env } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { afterEach, describe, expect, it, vi } from "vitest";
import example from "../examples/report-format-1.json";

let nextIp = 1;

// Each test sends from its own address, so the rate limiter's counters of one
// test do not reach another.
function post(body, { ip = `192.0.2.${nextIp++}`, type = "application/json", headers = {} } = {}) {
  return exports.default.fetch("https://bench.happydwarf.dev/reports", {
    method: "POST",
    headers: { "content-type": type, "cf-connecting-ip": ip, ...headers },
    body: typeof body === "string" ? body : JSON.stringify(body),
  });
}

function report(change = () => {}) {
  const r = structuredClone(example);
  r.series_id = crypto.randomUUID();
  change(r);
  return r;
}

async function message(response) {
  return (await response.json()).message;
}

function today() {
  return new Date().toISOString().slice(0, 10);
}

afterEach(() => vi.restoreAllMocks());

describe("a valid report", () => {
  it("is stored byte for byte with the day of receipt", async () => {
    const r = report();
    const body = JSON.stringify(r, null, 2);
    const response = await post(body);
    expect(response.status).toBe(201);
    const row = await env.DB.prepare("SELECT * FROM reports WHERE series_id = ?1").bind(r.series_id).first();
    expect(row.body).toBe(body);
    expect(row.format).toBe(1);
    expect(row.received_day).toBe(today());
    expect(row.anvil_version).toBe("1.5.0");
    expect(row.compared_plugin).toBe("bellows");
    expect(row.compared_version).toBe("0.10.0");
    expect(row.series_state).toBe("complete");
  });

  it("answers 200 without a write when the series arrives again", async () => {
    const r = report();
    expect((await post(r)).status).toBe(201);
    const again = await post(JSON.stringify(r) + "\n");
    expect(again.status).toBe(200);
    expect(await message(again)).toBe("This report has already arrived.");
    const row = await env.DB.prepare("SELECT body FROM reports WHERE series_id = ?1").bind(r.series_id).first();
    expect(row.body).toBe(JSON.stringify(r));
    const count = await env.DB.prepare("SELECT received FROM daily_counts WHERE day = ?1").bind(today()).first();
    expect(count.received).toBe(1);
  });

  it("accepts a stopped series", async () => {
    const r = report((r) => {
      r.series.state = "stopped";
      r.series.stop_reason = "player";
    });
    expect((await post(r)).status).toBe(201);
  });

  it("accepts a charset parameter", async () => {
    expect((await post(report(), { type: "application/json; charset=utf-8" })).status).toBe(201);
  });

  it("logs status, format and size only", async () => {
    const log = vi.spyOn(console, "log");
    const r = report();
    const body = JSON.stringify(r);
    await post(body, { ip: "198.51.100.7" });
    expect(log).toHaveBeenCalledTimes(1);
    const line = log.mock.calls[0].join(" ");
    expect(JSON.parse(line)).toEqual({ status: 201, format: 1, bytes: body.length });
    expect(line).not.toContain(r.series_id);
    expect(line).not.toContain("198.51.100.7");
  });
});

describe("check 1: size and type", () => {
  it("refuses a body over the limit by its length", async () => {
    const response = await post("x".repeat(262145));
    expect(response.status).toBe(413);
  });

  it("refuses a body over the limit while reading it", async () => {
    const big = new TextEncoder().encode(" ".repeat(262145));
    const stream = new ReadableStream({
      start(controller) {
        controller.enqueue(big);
        controller.close();
      },
    });
    const response = await exports.default.fetch("https://bench.happydwarf.dev/reports", {
      method: "POST",
      headers: { "content-type": "application/json", "cf-connecting-ip": "192.0.2.250" },
      body: stream,
      duplex: "half",
    });
    expect(response.status).toBe(413);
  });

  it("refuses another content type", async () => {
    expect((await post(report(), { type: "text/plain" })).status).toBe(415);
    expect((await post(report(), { type: "application/json; charset=latin1" })).status).toBe(415);
  });

  it("answers only POST /reports", async () => {
    expect((await exports.default.fetch("https://bench.happydwarf.dev/")).status).toBe(404);
    const get = await exports.default.fetch("https://bench.happydwarf.dev/reports");
    expect(get.status).toBe(405);
    expect(get.headers.get("allow")).toBe("POST");
  });
});

describe("check 2: per IP", () => {
  it("allows 5 requests per 60 s from one address", async () => {
    const ip = "203.0.113.9";
    for (let i = 0; i < 5; i++) expect((await post(report(), { ip })).status).toBe(201);
    const sixth = await post(report(), { ip });
    expect(sixth.status).toBe(429);
    expect((await post(report())).status).toBe(201);
  });
});

describe("check 3: per day", () => {
  it("answers 503 once the day's limit is reached", async () => {
    await env.DB.prepare("INSERT INTO daily_counts (day, received) VALUES (?1, 2000)").bind(today()).run();
    const response = await post(report());
    expect(response.status).toBe(503);
  });

  it("counts new reports only", async () => {
    await env.DB.prepare("INSERT INTO daily_counts (day, received) VALUES (?1, 1999)").bind(today()).run();
    expect((await post(report())).status).toBe(201);
    expect((await post(report())).status).toBe(503);
  });
});

describe("check 3: per day, in bytes", () => {
  it("answers 503 once the day's bytes are reached", async () => {
    await env.DB.prepare("INSERT INTO daily_counts (day, received, bytes) VALUES (?1, 1, ?2)")
      .bind(today(), Number(env.DAILY_BYTES_LIMIT))
      .run();
    expect((await post(report())).status).toBe(503);
  });

  it("adds a new report's bytes, and a repeat's not at all", async () => {
    const r = report();
    const body = JSON.stringify(r);
    expect((await post(body)).status).toBe(201);
    expect((await post(body)).status).toBe(200);
    const row = await env.DB.prepare("SELECT received, bytes FROM daily_counts WHERE day = ?1").bind(today()).first();
    expect(row).toEqual({ received: 1, bytes: new TextEncoder().encode(body).byteLength });
  });
});

describe("check 4: validation", () => {
  it("refuses a body that is not JSON", async () => {
    const response = await post("{");
    expect(response.status).toBe(422);
    expect(await message(response)).toBe("The report is not UTF-8 JSON.");
  });

  it("tells the player to update Anvil for an unknown format", async () => {
    const response = await post(report((r) => (r.format = 2)));
    expect(response.status).toBe(422);
    expect(await message(response)).toMatch(/format 2.*Update Anvil/);
  });

  it("names the place of a schema error", async () => {
    const response = await post(report((r) => (r.starts[3].arm = "both")));
    expect(response.status).toBe(422);
    expect(await message(response)).toMatch(/^The report does not match format 1: \/starts\/3\/arm/);
  });

  it("refuses a field outside the format", async () => {
    const response = await post(report((r) => (r.system.computer_name = "DESKTOP")));
    expect(response.status).toBe(422);
  });

  it("refuses a plugin field that is not declared", async () => {
    const response = await post(report((r) => (r.starts[2].plugins.bellows.fields["open_warmup.path"] = 1)));
    expect(response.status).toBe(422);
    expect(await message(response)).toMatch(/open_warmup.path of plugin bellows in start 3 is not declared/);
  });

  it("refuses a plugin field of the wrong kind", async () => {
    const response = await post(report((r) => (r.starts[2].plugins.bellows.fields["open_warmup.mode"] = "fast")));
    expect(response.status).toBe(422);
    expect(await message(response)).toMatch(/not a valid enum/);
  });

  it("refuses a start time outside the sane range", async () => {
    const response = await post(report((r) => (r.starts[0].time_ms = 999)));
    expect(response.status).toBe(422);
    expect(await message(response)).toMatch(/outside 1000-600000 ms/);
  });

  it("refuses a median that the rows do not give", async () => {
    const response = await post(report((r) => (r.result.warm.with.median_ms = 33000)));
    expect(response.status).toBe(422);
    expect(await message(response)).toMatch(/warm median or range with the plugin/);
  });

  it("refuses a difference that the medians do not give", async () => {
    const response = await post(report((r) => (r.result.difference.ms = -9000)));
    expect(response.status).toBe(422);
  });
});
