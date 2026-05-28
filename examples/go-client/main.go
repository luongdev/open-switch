// examples/go-client/osw-client — sample gRPC client for mod_open_switch.
//
// Exercises every RPC implemented through W3 against a running module.
// The module's default listen address is 0.0.0.0:50061 (matches
// deploy/freeswitch/conf/autoload_configs/open_switch.conf.xml).
//
// Build:
//
//	go build -o osw-client .
//
// Examples:
//
//	./osw-client -addr 127.0.0.1:50061 health
//	./osw-client originate -endpoints 'sofia/external/1000@example.com' -after-park
//	./osw-client originate -endpoints 'sofia/internal/1001@example.com' -dialplan-dest 9999
//	./osw-client hangup -uuid <CHANNEL_UUID>
//	./osw-client bridge -a <UUID_A> -b <UUID_B>
//	./osw-client execute -uuid <UUID> -app playback -args silence_stream://2000
//	./osw-client start-tts -uuid <UUID> -endpoint tts-edge:50062 -text 'Xin chao'
//	./osw-client transfer -uuid <UUID> -dest 9999
//	./osw-client set-vars -uuid <UUID> -k foo=bar -k bar=baz
//	./osw-client hold -uuid <UUID>
//	./osw-client unhold -uuid <UUID>
//	./osw-client subscribe -tiers TIER_1_CRITICAL,TIER_2_OPERATIONAL -event 'CHANNEL_*'
//
// This sample uses insecure (plaintext) gRPC. W4 lands TLS / mTLS and
// the Auth interceptor; once W4 ships, switch to grpc.NewClient(addr,
// grpc.WithTransportCredentials(creds)) and add a Bearer-token call
// option.
//
// SPDX-License-Identifier: AGPL-3.0-or-later
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"strings"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/protobuf/types/known/durationpb"

	controlpb "github.com/luongdev/open-switch/examples/go-client/pb/open_switch/control/v1"
)

// --- top-level dispatch ----------------------------------------------------

func main() {
	addr := flag.String("addr", "127.0.0.1:50061", "gRPC server address")
	tenant := flag.String("tenant", "demo-tenant", "tenant_id header")
	timeout := flag.Duration("timeout", 10*time.Second, "per-RPC deadline")
	flag.Parse()

	args := flag.Args()
	if len(args) == 0 {
		usage()
	}
	cmd, sub := args[0], args[1:]

	ctx, cancel := context.WithTimeout(context.Background(), *timeout)
	defer cancel()

	conn, err := grpc.NewClient(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Fatalf("dial %s: %v", *addr, err)
	}
	defer conn.Close()
	cli := controlpb.NewControlServiceClient(conn)

	hdr := func() *controlpb.RequestHeader {
		return &controlpb.RequestHeader{
			RequestId: fmt.Sprintf("osw-client-%d", time.Now().UnixNano()),
			TenantId:  *tenant,
		}
	}

	switch cmd {
	case "health":
		health(ctx, cli, hdr())
	case "originate":
		originate(ctx, cli, hdr(), sub)
	case "hangup":
		hangup(ctx, cli, hdr(), sub)
	case "hangup-many":
		hangupMany(ctx, cli, hdr(), sub)
	case "bridge":
		bridge(ctx, cli, hdr(), sub)
	case "execute":
		execute(ctx, cli, hdr(), sub)
	case "start-tts":
		startTts(ctx, cli, hdr(), sub)
	case "transfer":
		blindTransfer(ctx, cli, hdr(), sub)
	case "set-vars":
		setVariables(ctx, cli, hdr(), sub)
	case "hold":
		hold(ctx, cli, hdr(), sub)
	case "unhold":
		unhold(ctx, cli, hdr(), sub)
	case "subscribe":
		// SubscribeEvents is server-streaming — use a fresh context that
		// outlives the outer timeout (use parent-cancellable instead).
		bg, bgCancel := context.WithCancel(context.Background())
		defer bgCancel()
		subscribe(bg, cli, hdr(), sub)
	default:
		usage()
	}
}

func usage() {
	fmt.Fprintf(os.Stderr, `usage: %s [global-opts] <cmd> [cmd-opts]

global opts:
    -addr <host:port>   gRPC server (default 127.0.0.1:50061)
    -tenant <id>        tenant_id RequestHeader (default demo-tenant)
    -timeout <dur>      per-RPC deadline (default 10s)

cmds:
    health
    originate     -endpoints <ep[,ep...]> [-after-park | -dialplan-dest <ext> | -bridge-to <uuid>] [-caller-name|-caller-num] [-strategy SIMULTANEOUS|FAILOVER|MULTIPLE] [-timeout-sec <int>] [-var k=v]
    hangup        -uuid <uuid> [-cause CAUSE_STR] [-var k=v]
    hangup-many   -uuids <u1,u2,...> [-cause CAUSE_STR]
    bridge        -a <uuid_a> -b <uuid_b>
    execute       -uuid <uuid> -app <app> [-args <args>]
    start-tts     -uuid <uuid> [-endpoint <host:port>] [-rate 8000|16000] [-text <utterance>] [-var k=v] [-jitter-ms <ms>] [-preroll-ms <ms>]
    transfer      -uuid <uuid> -dest <extension> [-dialplan XML] [-context default]
    set-vars      -uuid <uuid> -k k=v -k ...
    hold          -uuid <uuid> [-uuid <uuid> ...]
    unhold        -uuid <uuid> [-uuid <uuid> ...]
    subscribe     [-tiers <t1,t2>] [-event <name|glob*>] [-since <seq>] [-node <id>]
`, os.Args[0])
	os.Exit(2)
}

// --- repeatedString flag --------------------------------------------------

type repeatedString []string

func (r *repeatedString) String() string     { return strings.Join(*r, ",") }
func (r *repeatedString) Set(v string) error { *r = append(*r, v); return nil }

// --- Health ---------------------------------------------------------------

func health(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader) {
	resp, err := cli.Health(ctx, &controlpb.HealthRequest{Header: hdr})
	if err != nil {
		log.Fatalf("Health: %v", err)
	}
	fmt.Printf("status:           %s\n", resp.GetStatus())
	fmt.Printf("module_version:   %s\n", resp.GetModuleVersion())
	fmt.Printf("freeswitch_ver:   %s\n", resp.GetFreeswitchVersion())
	fmt.Printf("active_channels:  %d\n", resp.GetActiveChannels())
	fmt.Printf("active_bugs:      %d\n", resp.GetActiveMediaBugs())
	fmt.Printf("events_emitted:   %d\n", resp.GetEventsEmittedTotal())
	fmt.Printf("subscribers:      %d\n", resp.GetSubscriberCount())
	fmt.Printf("tier1 fill:       %d%%  dropped=%d\n", resp.GetTier1RingFillPct(), resp.GetTier1DroppedTotal())
	fmt.Printf("tier2 fill:       %d%%  dropped=%d\n", resp.GetTier2RingFillPct(), resp.GetTier2DroppedTotal())
	fmt.Printf("tier3 fill:       %d%%  dropped=%d\n", resp.GetTier3RingFillPct(), resp.GetTier3DroppedTotal())
}

// --- Originate ------------------------------------------------------------

func originate(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("originate", flag.ExitOnError)
	endpoints := fs.String("endpoints", "", "comma-separated endpoint URIs")
	afterPark := fs.Bool("after-park", false, "park after answer (default if no after-* set)")
	dialplanDest := fs.String("dialplan-dest", "", "transfer to extension after answer")
	dialplanCtx := fs.String("dialplan-ctx", "default", "dialplan context")
	dialplanDP := fs.String("dialplan-dp", "XML", "dialplan name")
	bridgeTo := fs.String("bridge-to", "", "bridge to existing UUID after answer")
	callerName := fs.String("caller-name", "osw-client", "caller_id_name")
	callerNum := fs.String("caller-num", "0000000000", "caller_id_number")
	strategy := fs.String("strategy", "SIMULTANEOUS", "SIMULTANEOUS|FAILOVER|MULTIPLE")
	timeoutSec := fs.Int("timeout-sec", 60, "originate timeout (seconds)")
	var vars repeatedString
	fs.Var(&vars, "var", "channel variable k=v (repeatable)")
	_ = fs.Parse(args)

	if *endpoints == "" {
		log.Fatal("originate: -endpoints required")
	}

	req := &controlpb.OriginateRequest{
		Header:         hdr,
		Endpoints:      strings.Split(*endpoints, ","),
		Strategy:       parseStrategy(*strategy),
		CallerIdName:   *callerName,
		CallerIdNumber: *callerNum,
		Timeout:        durationpb.New(time.Duration(*timeoutSec) * time.Second),
		Variables:      parseKV(vars),
	}
	switch {
	case *bridgeTo != "":
		req.AfterAnswer = &controlpb.OriginateRequest_BridgeToUuid{BridgeToUuid: *bridgeTo}
	case *dialplanDest != "":
		req.AfterAnswer = &controlpb.OriginateRequest_Dialplan{
			Dialplan: &controlpb.DialplanTarget{
				Destination: *dialplanDest,
				Dialplan:    *dialplanDP,
				Context:     *dialplanCtx,
			},
		}
	default:
		// fallthrough: park
		_ = *afterPark
		req.AfterAnswer = &controlpb.OriginateRequest_Park{Park: true}
	}

	resp, err := cli.Originate(ctx, req)
	if err != nil {
		log.Fatalf("Originate: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("Originate error: %s %s", e.Type, e.Message)
	}
	fmt.Println(resp.GetChannelUuid())
}

func parseStrategy(s string) controlpb.OriginateRequest_Strategy {
	switch strings.ToUpper(s) {
	case "FAILOVER":
		return controlpb.OriginateRequest_FAILOVER
	case "MULTIPLE":
		return controlpb.OriginateRequest_MULTIPLE
	default:
		return controlpb.OriginateRequest_SIMULTANEOUS
	}
}

// --- Hangup --------------------------------------------------------------

func hangup(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("hangup", flag.ExitOnError)
	uuid := fs.String("uuid", "", "channel UUID")
	cause := fs.String("cause", "NORMAL_CLEARING", "FreeSWITCH cause code")
	var vars repeatedString
	fs.Var(&vars, "var", "channel variable k=v before hangup")
	_ = fs.Parse(args)

	if *uuid == "" {
		log.Fatal("hangup: -uuid required")
	}
	resp, err := cli.Hangup(ctx, &controlpb.HangupRequest{
		Header:    hdr,
		Uuid:      *uuid,
		Cause:     *cause,
		Variables: parseKV(vars),
	})
	if err != nil {
		log.Fatalf("Hangup: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("Hangup error: %s %s", e.Type, e.Message)
	}
	fmt.Println("OK")
}

// --- HangupMany ----------------------------------------------------------

func hangupMany(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("hangup-many", flag.ExitOnError)
	uuidsCSV := fs.String("uuids", "", "comma-separated UUIDs")
	cause := fs.String("cause", "NORMAL_CLEARING", "FreeSWITCH cause code")
	_ = fs.Parse(args)

	if *uuidsCSV == "" {
		log.Fatal("hangup-many: -uuids required")
	}
	resp, err := cli.HangupMany(ctx, &controlpb.HangupManyRequest{
		Header: hdr,
		Uuids:  strings.Split(*uuidsCSV, ","),
		Cause:  *cause,
	})
	if err != nil {
		log.Fatalf("HangupMany: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("HangupMany error: %s %s", e.Type, e.Message)
	}
	for _, u := range resp.GetHungupUuids() {
		fmt.Println(u)
	}
}

// --- Bridge --------------------------------------------------------------

func bridge(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("bridge", flag.ExitOnError)
	a := fs.String("a", "", "leg A UUID")
	b := fs.String("b", "", "leg B UUID")
	_ = fs.Parse(args)
	if *a == "" || *b == "" {
		log.Fatal("bridge: -a and -b required")
	}
	resp, err := cli.Bridge(ctx, &controlpb.BridgeRequest{Header: hdr, LegAUuid: *a, LegBUuid: *b})
	if err != nil {
		log.Fatalf("Bridge: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("Bridge error: %s %s", e.Type, e.Message)
	}
	fmt.Println(resp.GetBridgedUuid())
}

// --- Execute -------------------------------------------------------------

func execute(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("execute", flag.ExitOnError)
	uuid := fs.String("uuid", "", "channel UUID")
	app := fs.String("app", "", "dialplan app name (allow-list enforced server-side)")
	appArgs := fs.String("args", "", "app args")
	_ = fs.Parse(args)
	if *uuid == "" || *app == "" {
		log.Fatal("execute: -uuid and -app required")
	}
	resp, err := cli.Execute(ctx, &controlpb.ExecuteRequest{
		Header: hdr, Uuid: *uuid, App: *app, Args: *appArgs,
	})
	if err != nil {
		log.Fatalf("Execute: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("Execute error: %s %s", e.Type, e.Message)
	}
	fmt.Println(resp.GetResult())
}

// --- StartTts ------------------------------------------------------------

func startTts(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("start-tts", flag.ExitOnError)
	uuid := fs.String("uuid", "", "channel UUID")
	endpoint := fs.String("endpoint", "tts-edge:50062", "OpenSwitch MediaBridge endpoint reachable from FreeSWITCH")
	rate := fs.Uint("rate", 8000, "sample rate in Hz (8000 or 16000)")
	text := fs.String("text", "", "optional opening utterance")
	jitterMS := fs.Uint("jitter-ms", 0, "optional jitter buffer override in ms")
	prerollMS := fs.Uint("preroll-ms", 0, "optional preroll override in ms")
	var vars repeatedString
	fs.Var(&vars, "var", "TTS variable k=v (repeatable)")
	_ = fs.Parse(args)

	if *uuid == "" {
		log.Fatal("start-tts: -uuid required")
	}
	if *endpoint == "" {
		log.Fatal("start-tts: -endpoint required")
	}
	if *rate != 8000 && *rate != 16000 {
		log.Fatal("start-tts: -rate must be 8000 or 16000")
	}

	req := &controlpb.StartTtsRequest{
		Header:           hdr,
		ChannelUuid:      *uuid,
		UpstreamEndpoint: *endpoint,
		SampleRateHz:     uint32(*rate),
		StartMessage:     *text,
		Variables:        parseKV(vars),
	}
	if *jitterMS != 0 || *prerollMS != 0 {
		req.BufferOverride = &controlpb.TtsBufferOverride{
			JitterBufferMs: uint32(*jitterMS),
			PrerollMs:      uint32(*prerollMS),
		}
	}

	resp, err := cli.StartTts(ctx, req)
	if err != nil {
		log.Fatalf("StartTts: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("StartTts error: %s %s", e.Type, e.Message)
	}
	fmt.Printf("stream_id: %s\n", resp.GetStreamId())
	fmt.Printf("codec:     %s\n", resp.GetNegotiatedCodec())
	fmt.Printf("rate_hz:   %d\n", resp.GetNegotiatedSampleRateHz())
}

// --- BlindTransfer -------------------------------------------------------

func blindTransfer(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("transfer", flag.ExitOnError)
	uuid := fs.String("uuid", "", "channel UUID")
	dest := fs.String("dest", "", "destination extension")
	dp := fs.String("dialplan", "", "dialplan (empty → FS default 'XML')")
	cxt := fs.String("context", "", "context (empty → channel's current context)")
	_ = fs.Parse(args)
	if *uuid == "" || *dest == "" {
		log.Fatal("transfer: -uuid and -dest required")
	}
	resp, err := cli.BlindTransfer(ctx, &controlpb.BlindTransferRequest{
		Header: hdr, Uuid: *uuid, Destination: *dest, Dialplan: *dp, Context: *cxt,
	})
	if err != nil {
		log.Fatalf("BlindTransfer: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("BlindTransfer error: %s %s", e.Type, e.Message)
	}
	fmt.Println("OK")
}

// --- SetVariables --------------------------------------------------------

func setVariables(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("set-vars", flag.ExitOnError)
	uuid := fs.String("uuid", "", "channel UUID")
	var kv repeatedString
	fs.Var(&kv, "k", "variable k=v (repeatable, max 64 per server contract)")
	_ = fs.Parse(args)
	if *uuid == "" || len(kv) == 0 {
		log.Fatal("set-vars: -uuid and at least one -k required")
	}
	resp, err := cli.SetVariables(ctx, &controlpb.SetVariablesRequest{
		Header: hdr, Uuid: *uuid, Variables: parseKV(kv),
	})
	if err != nil {
		log.Fatalf("SetVariables: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("SetVariables error: %s %s", e.Type, e.Message)
	}
	fmt.Printf("OK (%d vars set)\n", len(kv))
}

// --- Hold ----------------------------------------------------------------

func hold(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("hold", flag.ExitOnError)
	var uuids repeatedString
	fs.Var(&uuids, "uuid", "channel UUID (repeatable)")
	_ = fs.Parse(args)
	if len(uuids) == 0 {
		log.Fatal("hold: at least one -uuid required")
	}
	resp, err := cli.Hold(ctx, &controlpb.HoldRequest{Header: hdr, Uuids: uuids})
	if err != nil {
		log.Fatalf("Hold: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("Hold error: %s %s", e.Type, e.Message)
	}
	for _, u := range resp.GetHeldUuids() {
		fmt.Println(u)
	}
}

// --- Unhold --------------------------------------------------------------

func unhold(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("unhold", flag.ExitOnError)
	var uuids repeatedString
	fs.Var(&uuids, "uuid", "channel UUID (repeatable)")
	_ = fs.Parse(args)
	if len(uuids) == 0 {
		log.Fatal("unhold: at least one -uuid required")
	}
	resp, err := cli.Unhold(ctx, &controlpb.UnholdRequest{Header: hdr, Uuids: uuids})
	if err != nil {
		log.Fatalf("Unhold: %v", err)
	}
	if e := resp.GetError(); e != nil && e.Type != controlpb.ErrorDetail_TYPE_UNSPECIFIED {
		log.Fatalf("Unhold error: %s %s", e.Type, e.Message)
	}
	for _, u := range resp.GetUnheldUuids() {
		fmt.Println(u)
	}
}

// --- SubscribeEvents -----------------------------------------------------

func subscribe(ctx context.Context, cli controlpb.ControlServiceClient, hdr *controlpb.RequestHeader, args []string) {
	fs := flag.NewFlagSet("subscribe", flag.ExitOnError)
	tiersCSV := fs.String("tiers", "", "comma-separated tiers (TIER_1_CRITICAL,...)")
	var events repeatedString
	fs.Var(&events, "event", "event name or prefix-glob (e.g. CHANNEL_*) — repeatable")
	since := fs.Uint64("since", 0, "since_seq (replay from this sequence exclusive)")
	node := fs.String("node", "", "filter by emitting node_id")
	var subs repeatedString
	fs.Var(&subs, "subclass", "subclass glob (e.g. osw.audit.*) — repeatable")
	_ = fs.Parse(args)

	req := &controlpb.SubscribeEventsRequest{
		Header:        hdr,
		SinceSeq:      *since,
		NodeId:        *node,
		EventNames:    events,
		SubclassGlobs: subs,
	}
	if *tiersCSV != "" {
		req.Tiers = strings.Split(*tiersCSV, ",")
	}

	stream, err := cli.SubscribeEvents(ctx, req)
	if err != nil {
		log.Fatalf("SubscribeEvents: %v", err)
	}
	log.Printf("subscribed: tiers=%v events=%v subclass=%v since=%d", req.Tiers, req.EventNames, req.SubclassGlobs, req.SinceSeq)
	for {
		env, err := stream.Recv()
		if err == io.EOF {
			log.Println("stream closed by server")
			return
		}
		if err != nil {
			log.Fatalf("Recv: %v", err)
		}
		fmt.Printf("seq=%d tier=%s name=%s node=%s subclass=%q ts=%s\n",
			env.GetSeq(),
			env.GetTier(),
			env.GetEventName(),
			env.GetNodeId(),
			env.GetSubclassName(),
			env.GetEmittedAt().AsTime().Format(time.RFC3339Nano),
		)
	}
}

// --- helpers --------------------------------------------------------------

func parseKV(items []string) map[string]string {
	if len(items) == 0 {
		return nil
	}
	out := make(map[string]string, len(items))
	for _, kv := range items {
		i := strings.IndexByte(kv, '=')
		if i <= 0 {
			log.Fatalf("bad k=v: %q", kv)
		}
		out[kv[:i]] = kv[i+1:]
	}
	return out
}
