-- Cleanup everything created by test_insert.sql.
-- Safe to run at any point: all statements use IF EXISTS.

DROP TABLE IF EXISTS t_insert_test_a CASCADE;
DROP TABLE IF EXISTS t_insert_test_b CASCADE;

DROP FUNCTION IF EXISTS assert_eq(TEXT, BIGINT, BIGINT);
DROP FUNCTION IF EXISTS assert_index_matches_seqscan(TEXT, TEXT[], TEXT[]);

DROP EXTENSION IF EXISTS corpussearch CASCADE;
