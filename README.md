# corpus_search

corpus_search is a library for efficient corpus search.


# Testing commands

```sql
create extension corpussearch;
create index my_ibpe_index on sentences using ibpe (text) with (
    tokenizer_path = '/var/lib/postgresql/tokenizer.json',
    normalize_mappings = '{".": "x", "/": "Z", "\\": "X", "`": "C"}'
);
SELECT pg_size_pretty(pg_relation_size('my_ibpe_index'));

-- with ibpe index
SET enable_seqscan = off; explain analyze select text from sentences where text ~ 'ho';

-- without index
SET max_parallel_workers_per_gather = 0; SET enable_seqscan = on; explain analyze select text from sentences where text ~ 'ho';

-- with ibpe index
SET enable_seqscan = off; explain analyze select text from sentences where text ~ 'si\.ta\.so\.ngi\.ta';

-- without index
SET max_parallel_workers_per_gather = 0; SET enable_seqscan = on; explain analyze select text from sentences where text ~ 'si\.ta\.so\.ngi\.ta';

drop extension corpussearch cascade;
```

# Benchmarks

Search time by tokenizer's vocab_size
```bash
vocab_size=2^10
indexsize = 124.9MB
o  -> 0.643146s
ho -> 0.152701s
ngixta ->  0.0463507s
kaxnanxho -> 0.0750657s

vocab_size=2^12
indexsize = 109.4MB
o  -> 0.739328s
ho -> 0.152018s
ngixta -> 0.0226821s
kaxnanxho -> 0.0748881s

vocab_size=2^14
indexsize = 99.5MB
o  -> 0.850803s
ho -> 0.175677s
ngixta -> 0.0386567s
kaxnanxho -> 0.10271s

vocab_size=2^16
indexsize = 94.3MB
o  -> 1.12015s
ho -> 0.305704s
ngixta -> 0.172394s
kaxnanxho -> 0.274248s
```

Best vocab_size = 2^12 (=4096).
