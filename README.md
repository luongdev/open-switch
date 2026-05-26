# open-switch

A FreeSWITCH module (`mod_open_switch.so`) that replaces both `mod_event_socket`
and ad-hoc gRPC bridges with a single, modern, observable interface:

- **Control plane** — gRPC server (mTLS, idempotent) for `Originate`, `Hangup`,
  `Bridge`, `Hold`, `Transfer`, `Execute`, `SetVariables`, …
- **Event plane** — FS events routed through tiered transports
  (Kafka / Redis Streams / NATS / gRPC stream) based on durability class
- **Media plane** — bidirectional gRPC streams per call for TTS / STT / voicebot
  via FreeSWITCH media bug, with built-in resampling

Status: **design phase**. Specs being drafted under `specs/`.

## License

[GNU Affero General Public License v3.0](LICENSE) (AGPL-3.0-or-later).

**What this means in practice**:

- You can **use, modify, and self-host** this module freely, including for
  internal business use.
- If you **distribute** modified versions, the modifications must be released
  under AGPL-3.0.
- If you **expose** functionality of this module (or a derivative) **over a
  network** to users — including as part of a commercial SaaS, hosted
  voicebot platform, contact-center service, or any other network-accessible
  product — you **must** make the complete corresponding source code of
  your service stack (the parts derived from or linking to this module)
  available to those users under AGPL-3.0.
- There is **no "private SaaS" loophole**. AGPL §13 closes it.

For commercial use that cannot comply with AGPL-3.0 (proprietary derivative,
closed-source SaaS), a **separate commercial license** must be negotiated
with the copyright holder. Open an issue or contact the maintainer.

## Relationship to open-tts

This module is developed alongside [open-tts](https://github.com/luongdev/open-tts)
and is consumed by it as a git submodule. The TTS use case drives the V1
design, but the module itself is generic FreeSWITCH ↔ gRPC infrastructure
and is usable independently for STT, voicebots, AMD, billing event pipelines,
or any other FreeSWITCH integration.

## Repository layout (planned)

```
open-switch/
├── LICENSE                 # AGPL-3.0
├── README.md
├── specs/                  # Design specs (Phase 1: S1-S6 incoming)
├── proto/                  # gRPC service + event schemas
├── src/                    # C++ module sources
├── deploy/                 # Dockerfile, helm, systemd
├── tests/                  # Unit + integration + load tests
└── CMakeLists.txt
```

Current state: stubs only. See `specs/` (incoming) for the design.

## Build

Build instructions will follow the spec drop in `specs/91-build-deploy.md`.
Target: Debian bookworm + FreeSWITCH 1.10.x + gRPC v1.69.x.

## Acknowledgements

Architectural patterns informed by [webitel/freeswitch-mod-grpc](https://github.com/webitel/freeswitch-mod-grpc)
(audio-bridge + media-bug-via-gRPC pattern) and the FreeSWITCH module API.
