# Benchmark receiver

The Cloudflare Worker behind `POST https://bench.happydwarf.dev/reports`. Anvil
Manager sends it one start-up benchmark report per series when the player
clicks **Send**; it keeps the report's bytes in D1 with the day of receipt and
nothing else. The design is section 9 of
[the benchmark spec](../../docs/spec/benchmark-v1.md); what the player is told
is [docs/benchmark-privacy.md](../../docs/benchmark-privacy.md).

The receiver is not part of an Anvil release: release-please ships nothing from
this folder, and changes here that do not touch the Manager use a hidden type
(`chore(bench-receiver): …`, `test(bench-receiver): …`).

| Path | What it is |
| --- | --- |
| `schemas/report-format-<n>.schema.json` | The JSON Schema of report format *n*, the contract between the Manager and the Worker |
| `examples/report-format-<n>.json` | A valid report of format *n*, used by the tests |
| `src/` | The Worker: `index.js` (the request, checks 1–5), `report.js` (check 4), `retention.js` (the Cron Trigger) |
| `scripts/compile-schemas.js` | Compiles the schemas into `src/generated/` (not in Git) |
| `migrations/` | D1 migrations: the tables and the views per format |
| `queries/` | Saved queries for maintainers |
| `test/` | Vitest in the Workers runtime, with a local D1 and rate limiter |

## Checks, in order

1. Body up to `MAX_BODY_BYTES` (413) and `application/json` (415).
2. 5 requests per 60 s per IP through the `RATE_LIMITER` binding (429).
3. At most `DAILY_LIMIT` new reports and `DAILY_BYTES_LIMIT` bytes per UTC day
   across all senders (503), so that one address cannot fill the database with
   the largest reports, read from the `daily_counts` row of the day: one row
   read per request
   instead of counting the day's reports.
4. The format's schema, then the plugin fields against `plugin_schemas` in the
   same report (every value declared, of its kind, or `null`), then
   plausibility: start positions 1…n, times between `MIN_START_MS` and
   `MAX_START_MS`, and the warm groups, medians, ranges and difference of
   `result` as the rows give them (422). An unknown format answers 422 with
   "Update Anvil".
5. `INSERT OR IGNORE` by `series_id`: 201 for a new series, 200 "already
   arrived" for a repeat, without a write.

Every response is `{"message": "…"}`; the Manager shows the message for 413
and 422. The numbers are `vars` and the `ratelimits` entry in
`wrangler.jsonc`. The Worker logs one line per request with status, format and
size; invocation logs are off, so no IP, body or series id reaches the logs.

The schema is compiled ahead of time with Ajv, because a Worker cannot
generate code at run time and the Free plan allows 10 ms of CPU per request: a
200-start report parses and passes check 4 in about 1 ms on a desktop CPU.

## Tests

Node.js 22 or newer:

```sh
npm ci
npm test         # compiles the schemas, then runs Vitest in workerd
npm run check    # wrangler deploy --dry-run: bundles and checks the configuration
```

CI runs both on every pull request (`.github/workflows/bench-receiver.yml`)
without a Cloudflare secret.

## Report formats

`format` *n* is exactly `schemas/report-format-<n>.schema.json`: any change to
Anvil's part of the report, an added field included, is a new format. Plugin
fields under `starts[].plugins.<id>.fields` never change the format; the
receiver checks them against the report's own `plugin_schemas`.

Format 1 can still change until Anvil 1.5 is released, in the pull request
that changes the Manager, and the receiver is deployed again. After that:

- A new format adds `schemas/report-format-<n>.schema.json`, an example, an
  entry in `FORMATS` in `src/report.js` with its plausibility checks, a
  migration with its `*_v<n>` views, and tests. The Manager's tests validate
  their reports against the same schema file: `anvil_manager_tests` builds
  the example's document from its series and writes
  `test/fixtures/manager-report-stopped.json`, which `test/report.test.js`
  checks (`ANVIL_UPDATE_FIXTURES=1` writes it again after a change).
- The receiver with the new format is deployed before the Manager that sends
  it is released (CONTRIBUTING.md, "A release that changes the report format").
- Old formats stay accepted until one is retired by removing it from
  `FORMATS`. Its stored reports and views stay.

The generated columns of `reports` (`anvil_version`, `compared_plugin`,
`compared_version`, `series_state`) read the envelope paths of format 1. A
format that moves them needs a migration that rebuilds those columns.

## Deployment

Deployment is manual. A maintainer runs Wrangler with an account-owned API
token in `CLOUDFLARE_API_TOKEN` (and `CLOUDFLARE_ACCOUNT_ID`) of the Happy
Dwarf account. The token needs Account › Workers Scripts › Edit (Workers Admin
for the first deployment, which creates the Worker), Account › D1 › Edit, and
Zone › Workers Routes › Edit on `happydwarf.dev`. Never commit the token.

### First deployment

1. Create the database, kept in the EU, and put the `uuid` it prints into `database_id` in
   `wrangler.jsonc` (a pull request; the id is not a secret):

   ```sh
   npx wrangler d1 create anvil-bench --jurisdiction eu
   ```

2. Apply the migrations:

   ```sh
   npx wrangler d1 migrations apply anvil-bench --remote
   ```

3. Deploy. The custom domain creates the `bench.happydwarf.dev` DNS record and
   certificate; there must be no other record for that name.

   ```sh
   npm run deploy
   ```

4. Send the example as a test report and expect 201, then 200 for a repeat:

   ```sh
   curl -i -X POST https://bench.happydwarf.dev/reports -H "content-type: application/json" --data-binary @examples/report-format-1.json
   ```

5. Delete the test report (below) once it has been checked.

### Later deployments

`npm run deploy` after the pull request is merged on `main`; first
`npx wrangler d1 migrations apply anvil-bench --remote` when it adds a
migration.

## Reading reports

Only members of the Cloudflare account can read reports: with Wrangler, in the
D1 console of the dashboard, or offline from an export. There is no admin UI
and no notification of new reports.

```sh
npx wrangler d1 execute anvil-bench --remote --file queries/recent.sql
npx wrangler d1 execute anvil-bench --remote --command "SELECT * FROM starts_v1 WHERE series_id = '<series id>'"
npx wrangler d1 export anvil-bench --remote --output anvil-bench.sql
```

| Query | Shows |
| --- | --- |
| `recent.sql` | the 50 most recent series with their result |
| `by-version.sql` | the warm difference per compared plugin version and Anvil version |
| `by-system.sql` | the warm difference by drive kind and memory |
| `discards.sql` | stop reasons, discard reasons and unclear starts |
| `cold-starts.sql` | cold starts with their signals and open cost per file |
| `plugin-fields.sql` | every plugin field over valid warm starts |
| `per-day.sql` | reports per day of receipt |

The views of format 1 are `starts_v1` (one row per start), `plugin_states_v1`
(one row per plugin per start), `plugin_fields_v1` (one row per field value)
and `results_v1` (one row per series). The tests run every saved query against
the example report.

## Deleting a report

A player asks by e-mail to `privacy@happydwarf.dev` with the report's series
id. A maintainer deletes it by hand and answers:

```sh
npx wrangler d1 execute anvil-bench --remote --command "DELETE FROM reports WHERE series_id = '<series id>'"
```

Bad reports are removed the same way. The Cron Trigger deletes reports 24
months after their day of receipt (`RETENTION_MONTHS`), daily at 03:17 UTC.
