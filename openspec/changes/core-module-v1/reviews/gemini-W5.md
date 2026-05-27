Warning: 256-color support not detected. Using a terminal with at least 256-color support is recommended for a better visual experience.
YOLO mode is enabled. All tool calls will be automatically approved.
YOLO mode is enabled. All tool calls will be automatically approved.
Ripgrep is not available. Falling back to GrepTool.
# W5 Gemini wave review

## Blockers
P1-1 `src/observability/metrics_server.cc:178`
`MetricsServer::HandleConnection` uses a blocking `recv()` without any timeout to read incoming HTTP requests. If a scraper connects but hangs or sends data too slowly to complete the headers, the single background thread will block indefinitely, causing `MetricsServer::Stop()` to deadlock on `loop_.join()` and completely preventing `Module::Shutdown` (and thus FreeSWITCH unloading).
Suggested fix: Configure a receive timeout on the accepted socket using `setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, ...)` before entering the `recv` loop, or use non-blocking I/O.

## Criticals
P2-1 `src/control/idempotency_cache.cc:113`
`IdempotencyCache::EvictIfNeeded()` performs an `O(N)` traversal of the entire `lru_list_` under the global cache mutex on every `Store()` operation to find expired entries. This will cause severe lock contention and latency spikes on the gRPC handler critical path under load, defeating the purpose of the cache.
Suggested fix: Remove the lazy TTL sweep loop from `EvictIfNeeded()`. The cache size is already safely bounded by `capacity_` via the `O(1)` LRU eviction, and `LookupOrReserve` properly evicts expired entries on access.

## Importants
P3-1 `src/core/module.cc:138`
`grpc_server_->SetIdempotencyCache()` is called *after* `grpc_server_->Start()`, creating a brief race window where the server is actively accepting gRPC connections but the cache is null. RPCs arriving during this window will bypass deduplication and execute live, which breaks idempotency guarantees if the client later retries the same request ID.
Suggested fix: Move the instantiation of `IdempotencyCache` and the `SetIdempotencyCache()` injection to before the `grpc_server_->Start()` call, as the cache construction only depends on `config_`.

## Nits
P4-1 `src/control/handlers/originate_handler.cc:120`
The boolean return value of `resp->SerializeToString()` is ignored when preparing the cache entry (this also occurs in the bridge and execute handlers). If serialization fails, an empty or corrupted string is cached, causing a silent fallback to live execution on subsequent hits when `ParseFromString()` fails.
Suggested fix: Check the return value of `SerializeToString()`. If it returns false, log a warning and call `cache->Cancel(request_id)` instead of `Store()` to cleanly release the in-flight reservation.
