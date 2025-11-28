-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION corpussearch" to load this file. \quit

CREATE FUNCTION ibpe_handler(internal)
    RETURNS index_am_handler
    AS 'MODULE_PATHNAME'
    LANGUAGE C;

-- Access method
CREATE ACCESS METHOD ibpe TYPE INDEX HANDLER ibpe_handler;
COMMENT ON ACCESS METHOD ibpe IS 'inverted BPE index access method';

-- Opclasses
CREATE OPERATOR CLASS text_ops
DEFAULT FOR TYPE text USING ibpe AS
    OPERATOR 1 ~ (text, text);
