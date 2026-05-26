# Security policy

## Supported versions

open-switch is in early development. Until 1.0.0, only the `main` branch
receives security patches.

## Reporting a vulnerability

Please **do not** open public GitHub issues for security vulnerabilities.

Email the maintainer with:

- Description of the issue
- Reproduction steps (FS version, module version, config)
- Potential impact (RCE, information disclosure, DoS, etc.)
- Suggested mitigation if known

You can expect an acknowledgement within 5 business days and a status
update within 14 days.

## Scope

In-scope for security reports:

- Vulnerabilities in `src/` (C++ module code)
- Vulnerabilities in proto definitions allowing unauthorized control of
  FreeSWITCH
- Bypasses of the eavesdrop guard on bot calls (privacy-critical)
- Authentication / authorization bypass in the gRPC control plane
- Memory-safety issues (use-after-free, double-free, out-of-bounds)
- Transport-layer issues (TLS misconfiguration, credential leakage)
- Build-supply-chain issues affecting the official Docker images

Out-of-scope:

- Vulnerabilities in upstream FreeSWITCH, gRPC, Redis, or other deps
  (report those upstream)
- Denial-of-service via misconfigured ACLs (operator responsibility)
- Issues in example configs that are clearly marked as samples

## Threat model summary

This module exposes a gRPC control plane that can originate calls, hang up
calls, transfer calls, and execute arbitrary FreeSWITCH dialplan applications.
It also bridges audio bidirectionally with external services. The module
runs in-process with FreeSWITCH, so any code-execution vulnerability gives
full control of the call switch.

Key assumed boundaries:

1. The gRPC port (default 50061) is reachable only by trusted callers, OR
   mTLS is enabled with strict cert verification.
2. The Redis instance(s) used for event transport are trusted; events are
   not signed.
3. Eavesdrop and call-recording controls are enforced at the dialplan layer
   in addition to module-level guards.

Reports that demonstrate breaking these boundaries are highly welcome.
