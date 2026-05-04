-- Cleanup everything created by test_vacuum.sql.
-- Safe to run at any point: all statements use IF EXISTS.

DROP TABLE IF EXISTS t_vacuum_test_d CASCADE;
DROP TABLE IF EXISTS t_vacuum_test_e CASCADE;
DROP TABLE IF EXISTS t_vacuum_test_f CASCADE;

DROP FUNCTION IF EXISTS assert_eq(TEXT, BIGINT, BIGINT);
DROP FUNCTION IF EXISTS assert_index_matches_seqscan(TEXT, TEXT[], TEXT[]);

DROP EXTENSION IF EXISTS corpussearch CASCADE;
