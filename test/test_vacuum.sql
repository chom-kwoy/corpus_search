\set ON_ERROR_STOP on
\set TOKENIZER_PATH '/var/lib/postgresql/tokenizer.json'
\set NORMALIZE_MAPPINGS '{".": "x", "/": "Z", "\\\\": "X", "`": "C"}'

create extension corpussearch;

-- Helper: assert that two counts are equal, raise if not.
CREATE OR REPLACE FUNCTION assert_eq(label TEXT, a BIGINT, b BIGINT) RETURNS void AS $$
BEGIN
    IF a IS DISTINCT FROM b THEN
        RAISE EXCEPTION 'FAIL [%]: expected % got %', label, b, a;
    END IF;
    RAISE NOTICE 'PASS [%]', label;
END;
$$ LANGUAGE plpgsql;

-- Helper: assert that a query via index returns the same rows as seq scan.
CREATE OR REPLACE FUNCTION assert_index_matches_seqscan(
    label TEXT,
    idx_results TEXT[],
    seq_results TEXT[]
) RETURNS void AS $$
DECLARE
    diff_count INT;
BEGIN
    SELECT count(*) INTO diff_count
    FROM (
        SELECT unnest(idx_results)
        EXCEPT
        SELECT unnest(seq_results)
    ) d;
    IF diff_count > 0 THEN
        RAISE EXCEPTION 'FAIL [%]: index returned % row(s) not in seqscan', label, diff_count;
    END IF;

    SELECT count(*) INTO diff_count
    FROM (
        SELECT unnest(seq_results)
        EXCEPT
        SELECT unnest(idx_results)
    ) d;
    IF diff_count > 0 THEN
        RAISE EXCEPTION 'FAIL [%]: seqscan returned % row(s) not in index', label, diff_count;
    END IF;

    RAISE NOTICE 'PASS [%]', label;
END;
$$ LANGUAGE plpgsql;

-- ============================================================
-- TEST D: VACUUM after delete from pending chain
--   ibpe_buildempty is called (empty index at CREATE INDEX time),
--   so every row goes through ibpe_insert into the pending chain.
--   After deleting a row and running VACUUM, ibpe_bulkdelete must
--   not throw ERROR, and index results must still match seqscan.
-- ============================================================
\echo '=== TEST D: vacuum after delete from pending ==='

CREATE TABLE t_vacuum_test_d (text TEXT);

CREATE INDEX idx_d ON t_vacuum_test_d USING ibpe (text) WITH (
    tokenizer_path = :'TOKENIZER_PATH',
    normalize_mappings = :'NORMALIZE_MAPPINGS'
);

INSERT INTO t_vacuum_test_d VALUES
    ('ho ngi ta'),
    ('si ta so ngi ta'),
    ('ta si ho ta');

-- Create a dead TID in the pending chain
DELETE FROM t_vacuum_test_d WHERE text = 'ho ngi ta';

-- VACUUM must not throw ERROR (previously: "ibpe_bulkdelete: Not implemented")
VACUUM t_vacuum_test_d;

-- D1: 'ngi' should find only 'si ta so ngi ta' (1 row)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_d WHERE text ~ 'ngi';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_d WHERE text ~ 'ngi';

    PERFORM assert_index_matches_seqscan('D1: ngi after vacuum (pending)', idx_rows, seq_rows);
    PERFORM assert_eq('D1: ngi row count', array_length(idx_rows, 1), 1);
END $$;

-- D2: 'ho' should find only 'ta si ho ta' (1 row)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_d WHERE text ~ 'ho';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_d WHERE text ~ 'ho';

    PERFORM assert_index_matches_seqscan('D2: ho after vacuum (pending)', idx_rows, seq_rows);
    PERFORM assert_eq('D2: ho row count', array_length(idx_rows, 1), 1);
END $$;

DROP TABLE t_vacuum_test_d;

-- ============================================================
-- TEST E: VACUUM after delete from bulk index
--   Rows are indexed via ibpe_build (bulk path).  Deleting a row
--   leaves a stale entry in the bulk SID pages; ibpe_bulkdelete
--   leaves those in place (needs_recheck=true covers correctness),
--   and ibpe_vacuumcleanup must not throw either.
-- ============================================================
\echo '=== TEST E: vacuum after delete from bulk index ==='

CREATE TABLE t_vacuum_test_e (text TEXT);

INSERT INTO t_vacuum_test_e VALUES
    ('ho ngi ta'),
    ('si ta so ngi ta'),
    ('ta si ho ta');

CREATE INDEX idx_e ON t_vacuum_test_e USING ibpe (text) WITH (
    tokenizer_path = :'TOKENIZER_PATH',
    normalize_mappings = :'NORMALIZE_MAPPINGS'
);

-- Create a dead TID in the bulk SID pages
DELETE FROM t_vacuum_test_e WHERE text = 'si ta so ngi ta';

VACUUM t_vacuum_test_e;

-- E1: 'ngi' should find only 'ho ngi ta' (1 row)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_e WHERE text ~ 'ngi';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_e WHERE text ~ 'ngi';

    PERFORM assert_index_matches_seqscan('E1: ngi after vacuum (bulk)', idx_rows, seq_rows);
    PERFORM assert_eq('E1: ngi row count', array_length(idx_rows, 1), 1);
END $$;

-- E2: 'si' should find only 'ta si ho ta' (1 row; 'si ta so ngi ta' deleted)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_e WHERE text ~ 'si';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_e WHERE text ~ 'si';

    PERFORM assert_index_matches_seqscan('E2: si after vacuum (bulk)', idx_rows, seq_rows);
    PERFORM assert_eq('E2: si row count', array_length(idx_rows, 1), 1);
END $$;

DROP TABLE t_vacuum_test_e;

-- ============================================================
-- TEST F: VACUUM after delete from mixed bulk + pending
--   Some rows are bulk-indexed; more are added as pending inserts.
--   One row is deleted from each section, then VACUUM runs.
--   Verifies that ibpe_bulkdelete correctly identifies dead
--   pending entries and that live rows from both sections
--   remain reachable after vacuum.
-- ============================================================
\echo '=== TEST F: vacuum after delete from mixed bulk+pending ==='

CREATE TABLE t_vacuum_test_f (text TEXT);

-- Bulk rows (indexed by ibpe_build)
INSERT INTO t_vacuum_test_f VALUES
    ('ho ngi ta'),
    ('si ta so ngi ta'),
    ('ta ho si');

CREATE INDEX idx_f ON t_vacuum_test_f USING ibpe (text) WITH (
    tokenizer_path = :'TOKENIZER_PATH',
    normalize_mappings = :'NORMALIZE_MAPPINGS'
);

-- Pending rows (indexed by ibpe_insert)
INSERT INTO t_vacuum_test_f VALUES ('ngi ho ta si');  -- will be deleted
INSERT INTO t_vacuum_test_f VALUES ('si ngi ho');     -- stays live

-- Delete one bulk row and one pending row
DELETE FROM t_vacuum_test_f WHERE text = 'ta ho si';
DELETE FROM t_vacuum_test_f WHERE text = 'ngi ho ta si';

VACUUM t_vacuum_test_f;

-- F1: 'ngi' -> 'ho ngi ta', 'si ta so ngi ta', 'si ngi ho' (3 rows)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_f WHERE text ~ 'ngi';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_f WHERE text ~ 'ngi';

    PERFORM assert_index_matches_seqscan('F1: ngi after mixed vacuum', idx_rows, seq_rows);
    PERFORM assert_eq('F1: ngi row count', array_length(idx_rows, 1), 3);
END $$;

-- F2: 'ho' -> 'ho ngi ta', 'si ngi ho' (2 rows; 'ta ho si' and 'ngi ho ta si' deleted)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_f WHERE text ~ 'ho';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_f WHERE text ~ 'ho';

    PERFORM assert_index_matches_seqscan('F2: ho after mixed vacuum', idx_rows, seq_rows);
    PERFORM assert_eq('F2: ho row count', array_length(idx_rows, 1), 2);
END $$;

-- F3: 'si' -> 'si ta so ngi ta', 'si ngi ho' (2 rows; 'ta ho si' deleted)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_vacuum_test_f WHERE text ~ 'si';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_vacuum_test_f WHERE text ~ 'si';

    PERFORM assert_index_matches_seqscan('F3: si after mixed vacuum', idx_rows, seq_rows);
    PERFORM assert_eq('F3: si row count', array_length(idx_rows, 1), 2);
END $$;

DROP TABLE t_vacuum_test_f;

-- ============================================================
-- Cleanup helpers
-- ============================================================
DROP FUNCTION assert_eq(TEXT, BIGINT, BIGINT);
DROP FUNCTION assert_index_matches_seqscan(TEXT, TEXT[], TEXT[]);
DROP EXTENSION corpussearch CASCADE;

\echo '=== ALL TESTS PASSED ==='
