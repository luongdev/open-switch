# Runtime smoke environment

A docker compose stack that brings up FreeSWITCH 1.10.12 with
`mod_open_switch` loaded, listening on `0.0.0.0:50061` for gRPC.

## Build

From the repo root, build the builder image (which carries the
compiled `mod_open_switch.so`):

```bash
docker buildx build \
  --platform linux/$(uname -m | sed 's/x86_64/amd64/;s/aarch64/arm64/') \
  --build-arg BUILD_TYPE=Release \
  --build-arg BASE_TAG=1.10.12-trixie \
  --target fs-builder \
  -f deploy/docker/Dockerfile.builder \
  --load -t open-switch/runtime:dev .
```

Then in this directory build the runtime image (just copies the
module + config onto the builder image):

```bash
cd examples/runtime
docker compose build
```

## Run

```bash
docker compose up -d
docker compose logs -f fs
```

You should see lines like:

```
osw-fs  | open_switch: gRPC server bound to 0.0.0.0:50061
```

## Smoke

In another terminal:

```bash
cd ../go-client
go build -o osw-client .

./osw-client health
# status: SERVING
# active_channels: 0
# ...

# Subscribe to all Tier-1 events in the background:
./osw-client subscribe -tiers TIER_1_CRITICAL &

# Originate a loopback call that parks (no external endpoint required):
./osw-client originate -endpoints 'loopback/9999' -after-park
# <prints channel UUID>

# Hang up:
./osw-client hangup -uuid <UUID>
```

## Stop / cleanup

```bash
docker compose down
docker rmi open-switch/runtime:fs        # optional
docker rmi open-switch/runtime:dev       # optional
```

## Notes

- The container runs FreeSWITCH in the foreground (`-nc -nf`) so
  `docker logs` shows FS console output.
- gRPC is **insecure** (plaintext). W4 lands TLS / mTLS + the auth
  interceptor; once W4 ships, configure `<param name="grpc_tls_*"`
  in `open_switch.conf.xml` and use TLS credentials in the client.
- For tests that need a real SIP peer, edit `sip_profiles/internal.xml`
  inside the container or mount your own.
