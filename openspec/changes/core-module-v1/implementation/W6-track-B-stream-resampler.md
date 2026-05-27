# W6 Track B — Bidi gRPC stream client + L16 framing + resampler

**Wave.** [W6 Media plane V1](W6-media-plane.md).
**Owner.** Sonnet sub-agent (claude-sonnet).
**Branch.** `implementation/wave6-track-b-stream-resampler` (off `main`).
**Phase.** 1 (parallel with Track A). Must merge before Track C starts.

Track B lands the gRPC client side of the `MediaBridge` bidi stream, the
`AudioFrame` POD/value type used as the in-process interchange between
FS bug callbacks and the stream, and the thin `switch_resample_t`
wrapper. No FS coupling at all on `Resampler` and `AudioFrame` (they
live in `osw_media` core lib); `StreamClient` may depend on osw_proto.

---

## Files in scope

**Create.**
- `include/osw/media/audio_frame.h` — POD-like value type (sample buffer
  view + metadata)
- `include/osw/media/resampler.h` — wrapper around `switch_resample_t`
- `include/osw/media/stream_client.h` — bidi gRPC stream client
- `src/media/audio_frame.cc`
- `src/media/resampler.cc`
- `src/media/stream_client.cc`
- `src/media/CMakeLists.txt` (NEW SUBDIR — coordinates with Track A; the
  first track to merge owns the file, the second adds its sources)
- `tests/unit/media/CMakeLists.txt` (NEW SUBDIR — same coordination)
- `tests/unit/media/audio_frame_test.cc`
- `tests/unit/media/resampler_test.cc`
- `tests/unit/media/stream_client_test.cc`

**Modify.**
- `src/CMakeLists.txt` — `add_subdirectory(media)` (idempotent — if Track
  A already added it, leave alone)
- `tests/unit/CMakeLists.txt` — `add_subdirectory(media)` (same)
- `openspec/changes/core-module-v1/FREESWITCH-FACTS.md` — append
  - `FF-033` — `switch_resample_t` lifecycle (alloc, write/read pattern,
    state preserved across frames, destroy)
  - `FF-034` — gRPC 1.74 `ClientReaderWriter` bidi semantics: WritesDone
    half-closes the send side, Read returns false when peer half-closes
    the receive side; cancellation is sticky.

> **Coordination with Track A** on the new `src/media/` and
> `tests/unit/media/` CMake subdirs: whichever PR merges second will hit
> a merge conflict if both add `media.cc` lists from scratch. Track B's
> sub-agent should rebase on Track A's merged commit if A lands first,
> or vice versa. Orchestrator resolves at merge time.

---

## `osw::media::AudioFrame`

```cpp
namespace osw::media {

/// Owning view over L16 PCM samples + metadata. Cheap to move; copying
/// duplicates the sample buffer (callers should move when possible).
class AudioFrame {
  public:
    AudioFrame() noexcept = default;
    AudioFrame(std::vector<std::int16_t> samples,
               std::uint32_t sample_rate_hz,
               std::uint32_t channels,
               std::uint64_t seq,
               std::uint64_t timestamp_samples) noexcept;

    AudioFrame(const AudioFrame&) = default;
    AudioFrame(AudioFrame&&) noexcept = default;
    AudioFrame& operator=(const AudioFrame&) = default;
    AudioFrame& operator=(AudioFrame&&) noexcept = default;

    [[nodiscard]] const std::int16_t* data() const noexcept { return samples_.data(); }
    [[nodiscard]] std::int16_t* data() noexcept { return samples_.data(); }
    [[nodiscard]] std::size_t sample_count() const noexcept { return samples_.size(); }
    [[nodiscard]] std::uint32_t sample_rate_hz() const noexcept { return sample_rate_hz_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
    [[nodiscard]] std::uint64_t timestamp_samples() const noexcept { return timestamp_samples_; }
    [[nodiscard]] std::uint32_t duration_samples() const noexcept {
        return channels_ > 0 ? static_cast<std::uint32_t>(samples_.size() / channels_) : 0;
    }
    [[nodiscard]] std::uint32_t duration_ms() const noexcept;  // computed

    /// Construct from a wire AudioFrame proto. Returns std::nullopt if
    /// payload size / channels don't match.
    static std::optional<AudioFrame> FromProto(
        const open_switch::media::v1::AudioFrame& proto,
        std::uint32_t sample_rate_hz,
        std::uint32_t channels) noexcept;

    /// Serialise into a wire proto. Codec is always PCM_S16LE in V1.
    void ToProto(open_switch::media::v1::AudioFrame* out) const noexcept;

  private:
    std::vector<std::int16_t> samples_;
    std::uint32_t sample_rate_hz_ = 0;
    std::uint32_t channels_ = 1;
    std::uint64_t seq_ = 0;
    std::uint64_t timestamp_samples_ = 0;
};

}  // namespace osw::media
```

### Tests (`audio_frame_test.cc`)

| # | Scenario | Expected |
|---|---|---|
| F1 | Construct + accessors | round-trip values intact |
| F2 | `duration_samples` for 160-sample mono frame | `160` |
| F3 | `duration_samples` for 320-sample stereo (channels=2) | `160` |
| F4 | `duration_ms` for 160 samples @ 8000 Hz mono | `20` |
| F5 | `ToProto` + `FromProto` round trip | bytes match; seq/ts preserved |
| F6 | `FromProto` with mismatched payload size | `nullopt` |
| F7 | Move-construct | sample buffer moved, source empty |

---

## `osw::media::Resampler`

```cpp
namespace osw::media {

/// Thin wrapper around switch_resample_t. Reuses internal FIR state
/// across calls (mandatory — see FF-033 to be added in this PR).
/// Not thread-safe; owned per StreamClient.
class Resampler {
  public:
    /// Allowed pairs in V1: (8000, 16000) and (16000, 8000). Any other
    /// pair returns nullptr (caller falls back to no-op + WARN log).
    static std::unique_ptr<Resampler> Create(int from_hz, int to_hz) noexcept;

    ~Resampler() noexcept;
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&&) noexcept = delete;
    Resampler& operator=(Resampler&&) noexcept = delete;

    /// Resample `in_samples` input samples into `out` (capacity =
    /// `out_cap` samples). Returns the number of output samples written.
    /// Returns 0 on FS-internal error (rare); caller may log + skip.
    std::size_t Process(const std::int16_t* in,
                        std::size_t in_samples,
                        std::int16_t* out,
                        std::size_t out_cap) noexcept;

    [[nodiscard]] int from_hz() const noexcept { return from_hz_; }
    [[nodiscard]] int to_hz() const noexcept { return to_hz_; }

  private:
    Resampler(switch_audio_resampler_t* res, int from_hz, int to_hz) noexcept;
    switch_audio_resampler_t* resampler_ = nullptr;
    int from_hz_ = 0;
    int to_hz_ = 0;
};

}  // namespace osw::media
```

### Implementation notes

- `Create` uses `switch_resample_create(&res, from_hz, to_hz, samples,
  SWITCH_RESAMPLE_QUALITY, 1)` (channels=1 — caller resamples each
  stereo channel separately).
- `Process` calls `switch_resample_process(res, in, in_samples)` to
  push samples through the filter, then `memcpy(out, res->to,
  res->to_len * sizeof(int16_t))` (or whatever the actual member is —
  read switch_resample.h in the base image).
- Validate that `out_cap >= res->to_len` before copying; otherwise log
  + return 0.
- Destroyed via `switch_resample_destroy(&res)` in the destructor.

### Tests (`resampler_test.cc`)

| # | Scenario | Expected |
|---|---|---|
| R1 | `Create(8000, 16000)` | non-null |
| R2 | `Create(16000, 8000)` | non-null |
| R3 | `Create(8000, 8000)` | non-null (no-op resampler is allowed; degenerate but valid) |
| R4 | `Create(8000, 24000)` | nullptr |
| R5 | `Create(48000, 16000)` | nullptr |
| R6 | Upsample 160 mono samples 8→16 kHz | 320 ± 4 samples written |
| R7 | Downsample 320 mono samples 16→8 kHz | 160 ± 4 samples written |
| R8 | Audio fidelity: 1 kHz sine 200 ms @ 8 kHz → upsample → RMS energy within ±0.5 dB of input | passes |
| R9 | Stateful across calls: feed sine in 5 chunks of 64 samples; output is contiguous (no phase discontinuity) | passes — measured by absence of high-frequency artifacts in FFT |

R8 + R9 may need synthesised sine wave helpers — add a small inline
helper in the test TU (not the production lib).

---

## `osw::media::StreamClient`

Wraps `open_switch::media::v1::MediaBridge::Stub::Stream(...)`.

```cpp
namespace osw::media {

/// Callbacks invoked from the reader thread. The reader thread is the
/// SAME thread for all of these — handlers should be fast or hand off
/// to a queue. Do not call any StreamClient method that takes mu_ from
/// inside these callbacks (Close is fine; SendAudio is fine).
struct StreamCallbacks {
    std::function<void(AudioFrame)> on_audio;          // TTS / voicebot rx
    std::function<void(open_switch::media::v1::Transcript)> on_transcript;
    std::function<void(open_switch::media::v1::AmdVerdict)> on_amd;
    std::function<void(open_switch::media::v1::Control)> on_control;
    std::function<void(grpc::Status)> on_done;         // reader exited
};

struct StreamConfig {
    std::string channel_uuid;
    std::string tenant_id;
    open_switch::media::v1::StreamStart::Purpose purpose;
    std::uint32_t sample_rate_hz;       // 8000 or 16000
    std::uint32_t channels = 1;
    open_switch::media::v1::AudioCodec codec =
        open_switch::media::v1::AudioCodec::PCM_S16LE;
    std::string traceparent;            // optional
    std::string start_message;          // optional (TTS opening line)
    std::map<std::string, std::string> variables;
};

class StreamClient {
  public:
    /// `channel` is created by the caller (single shared channel per
    /// endpoint is the usual pattern; do not new-up per stream).
    StreamClient(std::shared_ptr<grpc::Channel> channel,
                 StreamConfig config,
                 StreamCallbacks callbacks) noexcept;

    ~StreamClient() noexcept;  // calls Close() if still open

    StreamClient(const StreamClient&) = delete;
    StreamClient& operator=(const StreamClient&) = delete;
    StreamClient(StreamClient&&) = delete;
    StreamClient& operator=(StreamClient&&) = delete;

    /// Open the stream + send StreamStart + spawn reader thread. Blocks
    /// up to `open_deadline_ms` (default 5000) waiting for StreamReady.
    /// Returns the server's grpc::Status: OK on success, otherwise the
    /// reason (UNAVAILABLE, DEADLINE_EXCEEDED, INTERNAL, ...).
    grpc::Status Open(int open_deadline_ms = 5000) noexcept;

    /// Enqueue an AudioFrame into the bounded send ring (capacity 256).
    /// On overflow drops the OLDEST frame and increments an internal
    /// drop counter; returns false to signal the drop. Empty payloads
    /// are dropped silently.
    bool SendAudio(AudioFrame frame) noexcept;

    /// Send a Control message (Cancel / DTMF / BargeIn / Heartbeat).
    void SendControl(open_switch::media::v1::Control msg) noexcept;

    /// Half-close the send side + join the reader thread. Idempotent.
    /// Returns the final grpc::Status the reader observed.
    grpc::Status Close() noexcept;

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] std::uint64_t frames_sent() const noexcept;
    [[nodiscard]] std::uint64_t frames_dropped() const noexcept;
};

}  // namespace osw::media
```

### Internal threading

- `Open()` runs on the calling thread. Constructs `ClientContext`,
  calls `stub_->Stream(&context)`, writes `StreamStart`, reads
  `StreamReady`, then spawns `reader_thread_`.
- `reader_thread_` loops `stream->Read(&msg)` and dispatches via
  `StreamCallbacks`. On `Read` returning false: calls
  `stream->Finish()` → callback `on_done(status)` → exit.
- Send side uses a bounded MPSC ring (`AudioFrame` queue, capacity 256
  ≈ 5 s @ 20 ms ptime). A dedicated writer thread drains the ring and
  calls `stream->Write(proto_frame)`. Writer exits when (a) the ring
  is closed AND empty, or (b) a Write fails.
- `Close()` closes the ring, joins writer, calls
  `stream->WritesDone()`, joins reader.

### Tests (`stream_client_test.cc`)

Uses an in-process gRPC server (per W4C `metrics_server_test` pattern,
or the gRPC test util `grpc::testing::CreateChannelArgsForTest`).

| # | Scenario | Expected |
|---|---|---|
| S1 | `Open` against mock server returning `StreamReady` | returns OK in <100ms |
| S2 | `Open` against mock server that delays StreamReady > 5s | returns DEADLINE_EXCEEDED |
| S3 | `Open` against unreachable endpoint | returns UNAVAILABLE |
| S4 | `SendAudio` 100 frames | mock receives 100 AudioFrame in seq order |
| S5 | `SendAudio` 300 frames back-to-back (overflow capacity 256) | mock receives ≥256, frames_dropped() ≥ 44 |
| S6 | Mock server sends 50 AudioFrame back-to-back | `on_audio` invoked 50 times in order |
| S7 | Mock server sends Transcript | `on_transcript` invoked with text + final flag |
| S8 | `Close()` after S6 | returns OK, threads joined within 1 s |
| S9 | Mock server cancels mid-stream | reader observes cancel, `on_done(grpc::Status::CANCELLED)` fires once |
| S10 | Destructor with open stream | implicit Close() runs cleanly |

---

## FF entries to add

```
## FF-033 — `switch_resample_t` lifecycle (stateful across frames)

Construct via `switch_resample_create(&res, from_hz, to_hz, samples_per_frame,
SWITCH_RESAMPLE_QUALITY, channels)`. Returns SWITCH_STATUS_SUCCESS on
success; `res` is heap-allocated by FS.

Process via `switch_resample_process(res, in_samples, in_count)`. Output
samples land in `res->to` (member; check the actual field name in
switch_resample.h from base image — may be `res->resampler->result_buffer`
or similar). Length in `res->to_len`. The internal FIR filter state is
preserved across calls — DO NOT destroy and re-create between frames or
you get audible discontinuities at every frame boundary.

Destroy via `switch_resample_destroy(&res)` (note: takes &res, nulls
out the pointer).

V1 module wraps this in osw::media::Resampler; only 8↔16 kHz pairs
are exercised. Other rates return nullptr from Create() at the wrapper
level (the FS API itself accepts any rate — the restriction is policy).

## FF-034 — gRPC 1.74 ClientReaderWriter bidi semantics

For `service Foo { rpc Bidi(stream Req) returns (stream Resp); }`:
- Stub method returns `std::unique_ptr<grpc::ClientReaderWriter<Req,Resp>>`
  taking a `ClientContext*` (caller-owned, MUST outlive the stream).
- Write/Read are blocking by default. Write returns false on send-side
  shutdown OR on cancel. Read returns false on peer-half-close OR on
  cancel.
- `WritesDone()` half-closes the send side; the peer's Read returns the
  final pending messages then returns false.
- `Finish()` waits for the reader thread side and returns a final
  grpc::Status. MUST be called exactly once; calling on a still-open
  stream blocks until the peer finishes. Idempotent only if you guard
  with a flag.
- Cancellation: `ClientContext::TryCancel()` is async and sticky — once
  cancelled, subsequent Reads return false, Writes return false, and
  Finish returns grpc::Status::CANCELLED.
- Lifetime contract: stream MUST be destroyed BEFORE the ClientContext
  it was created against, or you get a use-after-free.

For the V1 media plane, the StreamClient owns both context and stream
as unique_ptrs and destroys stream first via Close() before letting
the context go out of scope.
```

---

## Acceptance criteria — wave gate

Phase 1 success requires Tracks A + B both green.

- Build: `docker buildx fs-builder` clean
- Tests: all new tests pass under ASAN amd64 + arm64; TSAN passes for
  the stress test in S5/S9 and the resampler R6/R7 (no data race in
  internal counters).
- Format: `clang-format` clean (silkeh/clang:18).
- The `media.proto` `AudioCodec::OPUS` enum value stays in the schema
  (proto-reserved) but the Resampler + StreamClient assert / log-warn
  if a peer requests OPUS — V1 only supports PCM_S16LE + G711 codec.

Commit message (no AI co-author trailers — contributor is @luongdev):
> `feat(media): AudioFrame + Resampler (switch_resample_t wrapper) + bidi gRPC StreamClient`

Push: `git push -u origin implementation/wave6-track-b-stream-resampler`.
