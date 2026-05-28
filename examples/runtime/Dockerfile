# examples/runtime/Dockerfile — minimal FreeSWITCH image with
# mod_open_switch wired in.
#
# Builds on top of the open-switch/runtime:dev image which already
# carries:
#   - FreeSWITCH 1.10.12 (from simplefs/open-switch-base:1.10.12-trixie)
#   - gRPC 1.74 + protobuf + abseil (pre-baked)
#   - /dist/mod/mod_open_switch.so (produced by deploy/docker/Dockerfile.builder)
#
# Use:
#   docker buildx build -t open-switch/runtime:fs -f examples/runtime/Dockerfile examples/runtime
#   docker run -d --rm --name osw-fs -p 50061:50061 open-switch/runtime:fs
#   ./examples/go-client/osw-client health
#
# Or via docker compose: cd examples/runtime && docker compose up -d.

ARG BUILDER_TAG=dev
FROM open-switch/runtime:${BUILDER_TAG}

# 1. Install the module into FreeSWITCH's mod dir. The base image's
#    FreeSWITCH prefix is /usr/local (binary in /usr/local/bin,
#    modules in /usr/local/mod, config in /usr/local/etc/freeswitch).
RUN install -m 0755 /dist/mod/mod_open_switch.so /usr/local/mod/mod_open_switch.so

# 2. Drop our modules.conf.xml + open_switch.conf.xml on top of the
#    base FreeSWITCH config tree. modules.conf.xml is COMPLETE — it
#    replaces the base FS one with a minimal set (mod_open_switch +
#    the loggers + sofia for outbound originate demos).
COPY modules.conf.xml /usr/local/etc/freeswitch/autoload_configs/modules.conf.xml
COPY open_switch.conf.xml /usr/local/etc/freeswitch/autoload_configs/open_switch.conf.xml
COPY event_socket.conf.xml /usr/local/etc/freeswitch/autoload_configs/event_socket.conf.xml
COPY dialplan/default.xml /usr/local/etc/freeswitch/dialplan/default.xml
COPY dialplan/public/00_osw_e2e.xml /usr/local/etc/freeswitch/dialplan/public/00_osw_e2e.xml
COPY sip_profiles/wsl.xml /usr/local/etc/freeswitch/sip_profiles/wsl.xml

# Keep demo RTP inside the docker-compose published UDP range.
RUN sed -i \
      -e 's|<!-- <param name="rtp-start-port" value="16384"/> -->|<param name="rtp-start-port" value="16384"/>|' \
      -e 's|<!-- <param name="rtp-end-port" value="32768"/> -->|<param name="rtp-end-port" value="16484"/>|' \
      /usr/local/etc/freeswitch/autoload_configs/switch.conf.xml

# 3. gRPC listen port from open_switch.conf.xml.
EXPOSE 50061

# 4. Run FS in foreground, no console, no fork.
#    `-nc`     — no console
#    `-nf`     — no forking (run in foreground; PID 1 for docker)
#    `-nonat`  — disable NAT detection (faster boot in containers)
ENTRYPOINT ["freeswitch", "-nc", "-nf", "-nonat"]
