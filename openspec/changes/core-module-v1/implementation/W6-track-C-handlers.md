# W6 Track C — Media RPC handlers (StartTts + StartStt + StartVoicebot + StopMediaStream)

**Wave.** [W6 Media plane V1](W6-media-plane.md).
**Owner.** Sonnet sub-agent (claude-sonnet).
**Branch.** `implementation/wave6-track-c-handlers` (off `main` AFTER
Tracks A + B have merged).
**Phase.** 2. Single-track. Cannot start before Phase 1 merges (needs
`BugManager::Attach` + `StreamClient::Open` APIs).

Track C lands the gRPC control-plane RPCs that operators call to start
media streams on a channel. Each handler combines the Track A
`MediaBugManager` (attach the FS media bug) with the Track B
`StreamClient` (open the bidi gRPC stream to the upstream TTS/STT/voicebot
service) and routes audio between them via per-purpose bug callbacks.

---

## Files in scope

**Modify (proto + skeleton).**
- `proto/open_switch/control/v1/control.proto` — add 4 RPCs +
  request/response messages
- `src/control/control_service_skeleton.h` — declare 4 method overrides
  + setters for `MediaBugManager*` and a `grpc::Channel` factory
- `src/control/control_service_skeleton.cc` — method bodies forward to
  per-handler TUs
- `src/control/handlers/unimplemented.cc` — remove the 4 method bodies
  (now in dedicated TUs)
- `src/control/CMakeLists.txt` — add new handler TUs
- `tests/unit/control/CMakeLists.txt` — register 4 new test binaries
- `include/osw/control/server.h` + `src/control/server.cc` — add
  `SetMediaBugManager` + `SetMediaChannelFactory` pass-throughs
- `src/core/module.cc` — construct `MediaBugManager` in Load step
  ~5b (between RpcMetrics and IdempotencyCache), inject into GrpcServer

**Create.**
- `include/osw/control/handlers/start_tts_handler.h`
- `include/osw/control/handlers/start_stt_handler.h`
- `include/osw/control/handlers/start_voicebot_handler.h`
- `include/osw/control/handlers/stop_media_stream_handler.h`
- `src/control/handlers/start_tts_handler.cc`
- `src/control/handlers/start_stt_handler.cc`
- `src/control/handlers/start_voicebot_handler.cc`
- `src/control/handlers/stop_media_stream_handler.cc`
- `src/control/handlers/media_bug_callbacks.h` + `.cc` — file-static
  callbacks per FF-003 (3 of them: read-tap, write-replace, voicebot
  duplex glue)
- `include/osw/control/active_media_streams.h` + `.cc` — per-Module
  registry keyed by `stream_id` → (BugHandle + StreamClient) for
  StopMediaStream lookup
- `tests/unit/control/start_tts_handler_test.cc`
- `tests/unit/control/start_stt_handler_test.cc`
- `tests/unit/control/start_voicebot_handler_test.cc`
- `tests/unit/control/stop_media_stream_handler_test.cc`

---

## Proto additions

Append to `service ControlService` in `control.proto`:

```protobuf
  // ── Media plane (W6) ─────────────────────────────────────────────────

  rpc StartTts(StartTtsRequest) returns (StartTtsResponse);
  rpc StartStt(StartSttRequest) returns (StartSttResponse);
  rpc StartVoicebot(StartVoicebotRequest) returns (StartVoicebotResponse);
  rpc StopMediaStream(StopMediaStreamRequest) returns (StopMediaStreamResponse);
```

Message definitions (append before the `// Reserved RPC slots` block):

```protobuf
// ─── Media — Start/Stop ──────────────────────────────────────────────

message StartTtsRequest {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string upstream_endpoint = 3;  // gRPC URL of TTS service
  uint32 sample_rate_hz = 4;     // 8000 or 16000; default 16000
  string start_message = 5;      // optional opening utterance
  map<string, string> variables = 10;  // passed through to TTS service
}

message StartTtsResponse {
  string stream_id = 1;  // UUIDv7 minted by handler
  open_switch.media.v1.AudioCodec negotiated_codec = 2;
  uint32 negotiated_sample_rate_hz = 3;
  ErrorDetail error = 99;
}

message StartSttRequest {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string upstream_endpoint = 3;
  uint32 sample_rate_hz = 4;     // default 16000
  string language = 5;           // BCP-47, e.g. "vi-VN"
  bool interim_results = 6;
  repeated string vocabulary_hints = 7;
  map<string, string> variables = 10;
}

message StartSttResponse {
  string stream_id = 1;
  open_switch.media.v1.AudioCodec negotiated_codec = 2;
  uint32 negotiated_sample_rate_hz = 3;
  ErrorDetail error = 99;
}

message StartVoicebotRequest {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string upstream_endpoint = 3;
  uint32 sample_rate_hz = 4;     // default 16000
  string start_message = 5;
  map<string, string> variables = 10;
}

message StartVoicebotResponse {
  // Voicebot is a single logical stream but produces TWO media bugs
  // internally (read tap + write replace). The stream_id identifies
  // the logical pair; StopMediaStream tears both down.
  string stream_id = 1;
  open_switch.media.v1.AudioCodec negotiated_codec = 2;
  uint32 negotiated_sample_rate_hz = 3;
  ErrorDetail error = 99;
}

message StopMediaStreamRequest {
  RequestHeader header = 1;
  string channel_uuid = 2;
  string stream_id = 3;
}

message StopMediaStreamResponse {
  bool was_active = 1;  // false if stream_id wasn't found (idempotent OK)
  ErrorDetail error = 99;
}
```

`media.proto` is already imported by `control.proto` (verify; if not,
add `import "open_switch/media/v1/media.proto";`).

---

## ActiveMediaStreams registry

Per-Module non-singleton (owned by `Module`, injected into handlers via
`ControlServiceSkeleton::SetActiveMediaStreams`). Keys: `stream_id`
(UUIDv7). Values: a small POD with the `BugHandle` (or two, for
voicebot) + `std::unique_ptr<osw::media::StreamClient>`.

```cpp
namespace osw::control {

struct ActiveMediaStream {
    std::string channel_uuid;
    std::string stream_id;
    open_switch::media::v1::StreamStart::Purpose purpose;
    std::vector<osw::media::BugHandle> bugs;     // 1 normally; 2 for voicebot
    std::unique_ptr<osw::media::StreamClient> client;
};

class ActiveMediaStreams {
  public:
    /// Insert + take ownership. Returns false if stream_id already exists.
    bool Insert(std::unique_ptr<ActiveMediaStream> s) noexcept;

    /// Remove + tear down. Idempotent; returns false if not present.
    bool Remove(std::string_view stream_id) noexcept;

    /// Remove every stream for a channel (called from CS_DESTROY hook
    /// alongside MediaBugManager::DetachAll).
    void RemoveForChannel(std::string_view channel_uuid) noexcept;

    [[nodiscard]] std::size_t Size() const noexcept;
};

}  // namespace osw::control
```

Removal order matters: call `client->Close()` BEFORE dropping `bugs`
(the bug callback may dereference the client; closing first ensures the
reader thread is joined before the client destructs).

---

## Handler flow (shared template)

All 4 handlers follow the same skeleton. Differences: purpose, bug
flags, callback wiring, and which side(s) of the stream are used.

```cpp
grpc::Status StartXxxHandler::Handle(
    grpc::ServerContext* ctx,
    const StartXxxRequest* req,
    StartXxxResponse* resp) {

    // 1. Validate request
    if (req->channel_uuid().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "channel_uuid required");
    }
    if (req->upstream_endpoint().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "upstream_endpoint required");
    }
    const std::uint32_t rate = req->sample_rate_hz() != 0
                                   ? req->sample_rate_hz()
                                   : kDefaultSampleRateHz;
    if (rate != 8000 && rate != 16000) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "sample_rate_hz must be 8000 or 16000");
    }

    // 2. Locate session (FF-016 → SessionGuard rwunlocks on scope exit)
    osw::control::SessionGuard sg(req->channel_uuid());
    if (!sg.valid()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "channel not found");
    }

    // 3. Build StreamClient + Open
    auto client = std::make_unique<osw::media::StreamClient>(
        channel_factory_(req->upstream_endpoint()),
        BuildStreamConfig(*req, rate),
        BuildCallbacks(req->channel_uuid(), /*stream_id later*/));
    grpc::Status open_status = client->Open(/*deadline_ms=*/5000);
    if (!open_status.ok()) {
        return open_status;
    }

    // 4. Attach media bug(s) via MediaBugManager
    osw::media::BugConfig cfg;
    cfg.purpose = kPurpose;            // per-handler constant
    cfg.fs_flags = kFlags;             // per-handler constant
    cfg.target_rate_hz = rate;
    cfg.tenant_id = req->header().tenant_id();
    auto attach = bug_mgr_->Attach(sg.session(), std::move(cfg));
    if (!attach.ok) {
        client->Close();
        return grpc::Status(attach.status_code, attach.error);
    }

    // 5. Mint stream_id, register in ActiveMediaStreams, populate
    //    response.
    const std::string stream_id = MintUuidV7();
    auto stream = std::make_unique<ActiveMediaStream>();
    stream->channel_uuid = req->channel_uuid();
    stream->stream_id = stream_id;
    stream->purpose = kPurposeProto;
    stream->bugs.push_back(std::move(attach.handle));
    stream->client = std::move(client);
    streams_->Insert(std::move(stream));

    resp->set_stream_id(stream_id);
    resp->set_negotiated_codec(open_switch::media::v1::AudioCodec::PCM_S16LE);
    resp->set_negotiated_sample_rate_hz(rate);
    osw::audit::Emit("control.media.start",
                     {{"channel_uuid", req->channel_uuid()},
                      {"purpose", PurposeName(kPurpose)},
                      {"stream_id", stream_id},
                      {"tenant_id", req->header().tenant_id()}});
    return grpc::Status::OK;
}
```

### Per-handler specifics

| Handler | Purpose | Bug flags | Callback role |
|---|---|---|---|
| **StartTts** | `kTtsPlayback` | `SMBF_WRITE_REPLACE` | Bug callback type `WRITE_REPLACE`: pop next `AudioFrame` from `StreamClient` rx queue (filled by `on_audio`), resample if needed, memcpy into `switch_frame_t::data` |
| **StartStt** | `kSttTranscribe` | `SMBF_READ_STREAM` | Bug callback type `READ`: copy `switch_frame_t::data` into `AudioFrame`, resample, `client->SendAudio(...)`. Transcripts arrive via `on_transcript` → emit Tier-2 event `osw.stt.transcript` (already-existing event plane API) |
| **StartVoicebot** | TWO bugs: `kVoicebotDuplexRead` (READ_STREAM) + `kVoicebotDuplexWrite` (WRITE_REPLACE) on same channel | as above | Read side: same as StartStt SendAudio path; write side: same as StartTts pop-from-rx-queue path |
| **StopMediaStream** | n/a | n/a | Just `streams_->Remove(stream_id)` — registry tear-down handles close + detach via destructors |

For voicebot, `Attach` is called TWICE on the same session (one for
read, one for write). Both BugHandles end up in
`ActiveMediaStream::bugs`. The second attach lands at INJECT rank
(>=MID_READ); stage-rank gate accepts it. On Remove, the `bugs` vector
clears in reverse — the write-side bug detaches first, then read-side,
then `client->Close()` runs via the registry's removal ordering hook.

> Recheck the removal-ordering claim against `ActiveMediaStreams::Remove`
> implementation: actual behaviour is `client->Close()` → `bugs.clear()`
> (vector destructor reverses element destruction). Order needs to be
> CLOSE → DETACH so the reader thread is joined before the bug callback
> can race with destruction. Implement accordingly.

---

## Bug callbacks (file-static per FF-003)

Three callbacks live in `media_bug_callbacks.cc`:

```cpp
extern "C" switch_bool_t OswStreamingReadTap(
    switch_media_bug_t* bug, void* user_data, switch_abc_type_t type) noexcept;

extern "C" switch_bool_t OswStreamingWriteReplace(
    switch_media_bug_t* bug, void* user_data, switch_abc_type_t type) noexcept;
```

`user_data` is a `BugCallbackContext*` (defined in Track A
`bug_manager.h`). Track C handler sets `ctx->user_cb` to point at one of
the above and `ctx->user_data` to a `StreamClient*` (non-owning — owned
by the ActiveMediaStream).

Body sketches:

```cpp
extern "C" switch_bool_t OswStreamingReadTap(
    switch_media_bug_t* bug, void* user_data, switch_abc_type_t type) noexcept {
    if (type != SWITCH_ABC_TYPE_READ && type != SWITCH_ABC_TYPE_READ_PING) {
        return SWITCH_TRUE;
    }
    auto* client = static_cast<osw::media::StreamClient*>(user_data);
    auto* frame = switch_core_media_bug_get_read_replace_frame(bug);  // verify FS API
    // (read tap: actually `get_read_frame`? Read FS source in builder image.)
    if (!frame || frame->datalen == 0) return SWITCH_TRUE;

    osw::media::AudioFrame af(
        /*samples=*/std::vector<std::int16_t>(
            reinterpret_cast<const std::int16_t*>(frame->data),
            reinterpret_cast<const std::int16_t*>(frame->data) +
                frame->samples * (frame->channels ? frame->channels : 1)),
        /*sample_rate=*/frame->rate,
        /*channels=*/frame->channels ? frame->channels : 1,
        /*seq=*/NextSeq(client),
        /*ts=*/CurrentTimestampSamples(client));
    client->SendAudio(std::move(af));
    return SWITCH_TRUE;
}
```

The WRITE_REPLACE callback is similar but takes an `AudioFrame` from
the client's rx queue (Track C adds a tiny per-StreamClient bounded
ring of incoming-frame buffers; alternatively, the StreamClient's
`on_audio` callback pushes into a member queue and the bug callback
pops). Document whichever direction is chosen.

---

## Acceptance criteria

| # | Scenario | Expected |
|---|---|---|
| C1 | StartTts → mock server → 100 AudioFrame from server | bug callback writes to `switch_frame_t` data in order |
| C2 | StartTts with empty channel_uuid | INVALID_ARGUMENT, no bug attached, no stream opened |
| C3 | StartTts against unreachable endpoint | UNAVAILABLE, bug NOT attached |
| C4 | StartStt → bug callback fires 50 times on synthesised mic frames | mock receives 50 AudioFrame in seq order, sample_rate matches |
| C5 | StartStt → mock sends 3 Transcripts (2 interim + 1 final) | 3 `osw.stt.transcript` Tier-2 events emitted |
| C6 | StartVoicebot → both read + write bugs attached | mock receives mic frames; mock-sent frames appear in write_replace bug callback |
| C7 | StopMediaStream(stream_id) | bug detached, stream closed, registry empty for that id, `was_active=true` |
| C8 | StopMediaStream(unknown_id) | `was_active=false`, OK status (idempotent) |
| C9 | Channel hangup mid-stream | `MediaBugManager::DetachAll` + `ActiveMediaStreams::RemoveForChannel` both run from CS_DESTROY; mock observes cancel; no leak |
| C10 | StartTts twice with same channel_uuid | second call returns ALREADY_EXISTS (per-purpose uniqueness) |
| C11 | StartTts then StartStt on same channel | both succeed (different purposes) |

Use the existing W3 handler test pattern (`OSW_TEST_FS_MOCK=1` +
`osw_audit_test_helpers` + mock seam in `include/osw/raii/fs_mock.h`).
Bug callbacks invoked manually in tests via the mock — no real FS bug
chain.

---

## Module wiring

Append to `src/core/module.cc::Load` between RpcMetrics construction
and IdempotencyCache:

```cpp
// 5.4. Media plane (W6).
bug_manager_ = std::make_unique<osw::media::MediaBugManager>();
bug_manager_->RegisterStateHandlers();  // installs CS_DESTROY hook

active_media_streams_ = std::make_unique<osw::control::ActiveMediaStreams>();

grpc_server_->SetMediaBugManager(bug_manager_.get());
grpc_server_->SetActiveMediaStreams(active_media_streams_.get());
grpc_server_->SetMediaChannelFactory(
    [](const std::string& endpoint) {
        return grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
    });
```

(For mTLS in V2: replace `InsecureChannelCredentials()` with a
`SslCredentials(...)` derived from operator config. Out of scope V1.)

Shutdown order in `Module::Shutdown` (insert in step 7.5 area):

```cpp
// Tear down media streams BEFORE the gRPC server drains: closing
// StreamClient half-closes upstream peers gracefully. After that,
// detach all bugs (defensive — most already gone via CS_DESTROY hook).
active_media_streams_.reset();
bug_manager_.reset();
```

---

## FF entries to add (if not covered by Track A/B)

Track A added FF-031, FF-032. Track B added FF-033, FF-034. Track C
likely doesn't need new FF entries — verify by reading the FS APIs
used in the bug callbacks (`switch_core_media_bug_get_read_replace_frame`,
`switch_core_media_bug_set_write_replace_frame`, etc.) against the
existing FF-001 / FF-006 references; add new entries if anything is
load-bearing-but-undocumented.

---

## Build + test (sub-agent runs locally before push)

Same docker build commands as Tracks A/B. Plus a quick smoke that
exercises an end-to-end TTS flow through the in-process mock server.

Commit message (no AI co-author trailers — contributor is @luongdev):
> `feat(control,media): StartTts + StartStt + StartVoicebot + StopMediaStream handlers`

Push: `git push -u origin implementation/wave6-track-c-handlers`.
