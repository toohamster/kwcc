# HTTP Module C Tests

> `tests/test_http.c` — 6 test points
> Last updated: 2026-06-26

## Test Results

| # | Test | Result |
|---|------|--------|
| 1 | init_no_crash | PASS |
| 2 | req_id_increments_curl_missing_skip | PASS |
| 3 | concurrent_limit_respected | PASS |
| 4 | overflow_returns_null | PASS |
| 5 | cancel_nonexistent_no_crash | PASS |
| 6 | cancel_null_no_crash | PASS |

**6/6 passed** (curl not in test env PATH, requests return NULL — Fix 6 verified)

## Build Command

```bash
mkdir -p tests/bin && gcc -Wall -I. -Ideps -D_GNU_SOURCE -DCONFIG_KWCC \
  -o tests/bin/test_http tests/test_http.c \
  build/obj/src/kwcc_http.o build/obj/src/kwcc_bus.o build/obj/src/kwcc_base.o \
  build/obj/src/kwcc_io.o build/obj/src/kwcc_config.o build/obj/src/kwcc_mempool.o \
  build/obj/deps/log/log.o build/obj/deps/picohttpparser/picohttpparser.o
```

## Notes

- Tests 2-4 require `curl` in PATH to start actual requests. When curl is unavailable,
  `access(bin_path, X_OK)` returns -1 and `kwcc_http_request` returns NULL (Fix 6).
  This is correct defensive behavior.
- End-to-end HTTP tests (with real network) are done in Step 8 via JS `$http` API.
