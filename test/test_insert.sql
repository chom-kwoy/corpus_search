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
-- Uses EXCEPT in both directions so order doesn't matter.
CREATE OR REPLACE FUNCTION assert_index_matches_seqscan(
    label TEXT,
    idx_results TEXT[],
    seq_results TEXT[]
) RETURNS void AS $$
DECLARE
    diff_count INT;
BEGIN
    -- rows in idx but not in seq
    SELECT count(*) INTO diff_count
    FROM (
        SELECT unnest(idx_results)
        EXCEPT
        SELECT unnest(seq_results)
    ) d;
    IF diff_count > 0 THEN
        RAISE EXCEPTION 'FAIL [%]: index returned % row(s) not in seqscan', label, diff_count;
    END IF;

    -- rows in seq but not in idx
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
-- TEST A: ibpe_insert into an empty index
--   ibpe_buildempty is called (no rows at CREATE INDEX time),
--   so every row goes through ibpe_insert.
--   Tests: first insert creates the pending chain from scratch.
--
--   All patterns use Korean romanizations known to map onto
--   tokens in this vocabulary: 'ngi', 'ho', 'si', 'ta'.
-- ============================================================
\echo '=== TEST A: insert into empty index ==='

CREATE TABLE t_insert_test_a (text TEXT);

CREATE INDEX idx_a ON t_insert_test_a USING ibpe (text) WITH (
    tokenizer_path = :'TOKENIZER_PATH',
    normalize_mappings = :'NORMALIZE_MAPPINGS'
);

INSERT INTO t_insert_test_a VALUES
    ('ho ngi ta'),
    ('si ta so ngi ta'),
    ('ta si ho ta');

-- A1: 'ngi' appears in rows 1 and 2 only
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_insert_test_a WHERE text ~ 'ngi';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_insert_test_a WHERE text ~ 'ngi';

    PERFORM assert_index_matches_seqscan('A1: ngi matches 2 pending rows', idx_rows, seq_rows);
    PERFORM assert_eq('A1: ngi row count', array_length(idx_rows, 1), 2);
END $$;

-- A2: 'ho' appears in rows 1 and 3 only
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_insert_test_a WHERE text ~ 'ho';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_insert_test_a WHERE text ~ 'ho';

    PERFORM assert_index_matches_seqscan('A2: ho matches 2 pending rows', idx_rows, seq_rows);
    PERFORM assert_eq('A2: ho row count', array_length(idx_rows, 1), 2);
END $$;

DROP TABLE t_insert_test_a;

-- ============================================================
-- TEST B: ibpe_insert after a bulk ibpe_build
--   ibpe_build runs over initial rows, then ibpe_insert
--   appends to the pending chain for subsequent inserts.
--   Tests:
--     B1 - pending row found for token shared with bulk index
--     B2 - pending row found for token NOT in bulk index
--          ('ngi.ta' is absent from bulk, present in pending)
--     B3 - bulk rows still found correctly after inserts
--     B4 - count matches seqscan across both bulk and pending
-- ============================================================
\echo '=== TEST B: insert after bulk build ==='

CREATE TABLE t_insert_test_b (text TEXT);

-- Bulk rows: none contain 'ngi.ta'
INSERT INTO t_insert_test_b VALUES
    ('ho ngi ta'),
    ('si ta so ngi ta'),
    ('ta ho si'),
    ('si ta si ho'),
    ('ngi ta ho');

CREATE INDEX idx_b ON t_insert_test_b USING ibpe (text) WITH (
    tokenizer_path = :'TOKENIZER_PATH',
    normalize_mappings = :'NORMALIZE_MAPPINGS'
);

-- Pending inserts
INSERT INTO t_insert_test_b VALUES ('ho si ta');       -- B1: 'ho' in bulk and pending
INSERT INTO t_insert_test_b VALUES ('ngi.ta ngi ho');  -- B2: 'ngi.ta' only in pending
INSERT INTO t_insert_test_b VALUES ('si ngi ta');      -- B4: 'si' in bulk and pending

-- B1: 'ho' exists in bulk and in the first pending row
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_insert_test_b WHERE text ~ 'ho';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_insert_test_b WHERE text ~ 'ho';

    PERFORM assert_index_matches_seqscan('B1: ho finds bulk+pending rows', idx_rows, seq_rows);
END $$;

-- B2: 'ngi.ta' is only in the second pending row (no bulk rows contain it)
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_insert_test_b WHERE text ~ 'ngi\.ta';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_insert_test_b WHERE text ~ 'ngi\.ta';

    PERFORM assert_index_matches_seqscan('B2: ngi.ta found via pending only', idx_rows, seq_rows);
    PERFORM assert_eq('B2: ngi.ta row count', array_length(idx_rows, 1), 1);
END $$;

-- B3: bulk rows are still accessible after pending inserts
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_insert_test_b WHERE text ~ 'ngi';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_insert_test_b WHERE text ~ 'ngi';

    PERFORM assert_index_matches_seqscan('B3: ngi finds bulk and pending rows', idx_rows, seq_rows);
END $$;

-- B4: count matches seqscan for a pattern spanning bulk and pending
DO $$
DECLARE
    idx_count BIGINT;
    seq_count BIGINT;
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT count(*) INTO idx_count FROM t_insert_test_b WHERE text ~ 'si';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT count(*) INTO seq_count FROM t_insert_test_b WHERE text ~ 'si';

    PERFORM assert_eq('B4: si count matches seqscan', idx_count, seq_count);
END $$;

-- ============================================================
-- TEST C: Multi-page pending chain
--   Insert enough rows to force the pending chain to span
--   multiple pages (a page holds ~500 ibpe_pending_entry
--   records; 100 rows of ~8 tokens each => ~800 entries => 2
--   pages minimum).
--   Sentences use 'ngi' and 'ho' so queries are predictable.
-- ============================================================
\echo '=== TEST C: multi-page pending chain ==='

INSERT INTO t_insert_test_b
    SELECT 'ngi ho ta si ngi ' || g
    FROM generate_series(1, 100) g;

-- C1: 'ngi' must span bulk rows + all pending pages
DO $$
DECLARE
    idx_count BIGINT;
    seq_count BIGINT;
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT count(*) INTO idx_count FROM t_insert_test_b WHERE text ~ 'ngi';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT count(*) INTO seq_count FROM t_insert_test_b WHERE text ~ 'ngi';

    PERFORM assert_eq('C1: ngi count matches seqscan (bulk+multi-page pending)', idx_count, seq_count);
END $$;

-- C2: 'ho' spans bulk + all pending pages
DO $$
DECLARE
    idx_count BIGINT;
    seq_count BIGINT;
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT count(*) INTO idx_count FROM t_insert_test_b WHERE text ~ 'ho';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT count(*) INTO seq_count FROM t_insert_test_b WHERE text ~ 'ho';

    PERFORM assert_eq('C2: ho count matches seqscan (bulk+multi-page pending)', idx_count, seq_count);
END $$;

-- C3: 'ngi.ta' still found in its specific pending row among 100+ new rows
DO $$
DECLARE
    idx_rows TEXT[];
    seq_rows TEXT[];
BEGIN
    SET enable_seqscan = off;
    SET enable_bitmapscan = on;
    SELECT array_agg(text ORDER BY text) INTO idx_rows
    FROM t_insert_test_b WHERE text ~ 'ngi\.ta';

    SET enable_seqscan = on;
    SET enable_bitmapscan = off;
    SELECT array_agg(text ORDER BY text) INTO seq_rows
    FROM t_insert_test_b WHERE text ~ 'ngi\.ta';

    PERFORM assert_index_matches_seqscan('C3: ngi.ta still found after multi-page insert', idx_rows, seq_rows);
    PERFORM assert_eq('C3: ngi.ta row count unchanged', array_length(idx_rows, 1), 1);
END $$;

DROP TABLE t_insert_test_b;

-- ============================================================
-- Cleanup helpers
-- ============================================================
DROP FUNCTION assert_eq(TEXT, BIGINT, BIGINT);
DROP FUNCTION assert_index_matches_seqscan(TEXT, TEXT[], TEXT[]);
DROP EXTENSION corpussearch CASCADE;

\echo '=== ALL TESTS PASSED ==='
