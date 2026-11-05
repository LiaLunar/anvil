-- The bytes stored per UTC day, for the daily limit in bytes: a report may be
-- 256 KB, so a limit in reports alone would let one address store 500 MB a day.
ALTER TABLE daily_counts ADD COLUMN bytes INTEGER NOT NULL DEFAULT 0;
