# OBS Mobile Controller ("Better OBS Blade") -- Research Brief

> Three deep-research passes, all adversarially fact-checked, 2026-08-01.
> Feeds the "Build order" in concept.md (build #2 = the controller).
> Likely lands as **ControlX mobile** (ControlX Ph2 = the Stream Deck relay), powered by a
> StrmrX cloud relay + paired with the DockX plugin. Not necessarily a new siloed app.

## The one-line verdict
The controller's FEATURE list is now table stakes (a fresh rival, Control OBS, already ships
almost all of it). The winning move is not "a better Blade in a vacuum" -- it's the **integrated
StrmrX play**: a cross-platform tablet-first HYBRID (live dashboard + customizable button grid)
that is the mobile face of an ecosystem (DockX plugin + cloud relay + account + wallet) doing
things a websocket-only app structurally cannot. Differentiate on execution, reliability,
zero-setup, IRL/away-from-desk, and plugin integration -- not on novelty.

## What to build (demand + design, verified)
- **Model: HYBRID.** Live dashboard (scenes, per-source audio VU + mute, live stream health)
  fused with a configurable button grid. Evidence supports it; pure-grid and pure-remote apps
  each cover only half.
- **Must-have baseline (DOA if missing):** tap-to-switch scenes, per-source volume + mute, live
  stream health (FPS/CPU/bitrate/dropped frames). Standard value prop, not a differentiator.
- **Highest-volume proven use case:** repurpose a phone/old tablet as a cheaper, bigger,
  more configurable Stream Deck (scene switching + soundboard) to dodge Elgato hardware.
- **Top pain points to beat (ranked):** (1) Wi-Fi disconnect / manual reconnect mid-stream ->
  resilient auto-reconnect; (2) confusing setup + basics behind paywalls (Touch Portal free tier
  caps 8 buttons / 2 pages, $13.99 Pro); (3) buttons that fire blind -> reflect LIVE OBS state
  (button icon/color changes on mute/stream toggle).
- **Underserved contexts to target:** IRL / away-from-desk control; live scene-preview mirroring
  on the tablet while streaming (least-solved, most differentiating); controlling OBS on a
  second / remote PC.
- **Platform wedge:** Deckboard = Android-only; Control OBS + ProducerPad = iOS-first.
  A genuinely cross-platform tablet-first app is itself an opening.
- Users treat these as essential infrastructure ("would not stream without it") -> real, sticky
  market, but a newcomer must clearly out-execute.

## The competitive reality (honest)
- **OBS Blade** (the famous free one) is effectively ABANDONED: last real release early 2023,
  pulled from Google Play Dec 2024. Open lane at the top.
- **BUT Control OBS** (by AntiScuff, v0.1.2, July 2026) already ships: hybrid dashboard+grid,
  one-tap "Smart Mute All" panic mute, per-source VU meters, stream-health monitoring, and
  remote-over-internet control with NO port forwarding (browser-source "Bridge ID" via
  proxy.controlobs.com/bridge). **ProducerPad** (iOS) also ships VU meters + faders + NDI
  program/preview. So the "killer features" are commoditized. Control OBS is early (v0.1.2) ->
  execution quality is beatable, but we are NOT first.
- **Stream Deck Mobile** (Elgato): button-grid, 6 keys free then pay up to 64. Static macros,
  no live state.

## Our defensible edges (only StrmrX can do these well)
1. **DockX plugin pairing.** The controller talks to the DockX plugin already in OBS, not just
   raw websocket. -> zero-setup pairing brokered by the StrmrX account (kills the #1 pain: no
   websocket config), deeper telemetry, and control of DockX's own features (layouts, scene
   folders, locks) from the phone. Websocket-only rivals can't touch this.
2. **Push health alerts to the phone even when the app is closed.** Server relay + push infra ->
   "your stream dropped / disk full / frames spiking" notification while you're AFK. Websocket
   apps can't push when backgrounded. Genuinely novel + high value.
3. **Cross-platform tablet-first** (iOS + Android) where rivals are split.
4. **The StrmrX ecosystem:** one account, wallet, family look; the controller is a doorway to
   the whole suite.
5. **Reliability + reconnection done right** (the #1 complaint) and **IRL/away-from-desk**
   positioning (proven demand, underserved).

## Monetization + cloud (verified)
- **Local features are forkable, not a moat.** Free GPL plugins already do local backup/export
  (Exeldro's Scene Collection Manager). The DockX plugin funnels; it never gates.
- **The moat is server-side/account-bound:** remote-over-internet relay, push alerts, multi-PC,
  cloud sync of layouts/decks, OBS-anywhere backup/restore, metered AI. All fork-proof.
- **Precedent: Aitum** -- free GPL OBS plugins (Multistream, Vertical) funnel into a paid app at
  **$4.99/mo or $49.99/yr**. Exactly StrmrX's planned structure.
- **Price anchors:** hobbyist automation ~$5/mo; semi-pro suites ~$15.75-27/mo (Streamlabs Ultra
  $27/mo or $189/yr; gates storage 1GB free vs 10GB paid).
- **Cloud storage economics:** object storage is cheap (~$0.007-0.015/GB/mo). Cloudflare **R2 =
  $0.015/GB/mo with ZERO egress** -> the correct backbone for a restore-heavy "OBS-anywhere"
  product. On S3, egress is the killer (~$900 to pull 10TB vs $0 on R2).

### Proposed tiers (draft, hobbyist -> semi-pro)
- **Free:** controller + DockX plugin, full local-network control (scenes, audio, health,
  button grid), small sync/backup quota (~1GB), watermark where visible.
- **~$5/mo entry:** remote-over-internet control from anywhere + push health alerts + unlimited
  layout/deck sync + bigger backup quota.
- **~$15/mo semi-pro:** OBS-anywhere full media backup/restore (tiered storage upsell,
  25-100GB), multi-PC control.
- **AI:** credit-wallet metered add-ons on top (NOT bundled free).

### Paid build order (revenue x defensibility)
1. Cross-machine settings/layout/deck sync (fork-proof, cheap, high value)
2. OBS-anywhere full backup/restore on R2 (fork-proof, storage upsell)
3. Metered AI via the credit wallet (fork-proof, account-bound)

## Open gaps / next research
- **AI section is thin** -- the AI research angle got dropped in verification and needs its own
  pass before we commit any AI features. Plausible value: smart stream-health alerts, a "director"
  auto-switch, auto-clip; all UNVALIDATED. Meter on the wallet (per-minute vs per-render TBD).
- No verified data on: haptics, Apple Watch/wearable glance, voice control, landscape-vs-portrait
  preference, real churn/retention numbers, or Control OBS's actual execution quality (latency,
  reliability, ergonomics at v0.1.2) -- worth a hands-on teardown before we commit.
- Media-path remapping on cross-machine restore is the hard technical part Streamlabs solved and
  the free Exeldro plugin only half-handles -- scope it before promising OBS-anywhere.

## Do-not-repeat (claims that got refuted in fact-check)
- OBS Blade is NOT pure-donation (it has cosmetic/tip IAPs).
- OBS Relay is NOT verifiably "free forever, no limits."
- Streamlabs did NOT sustain "unlimited free" cloud backup (now a gated 1GB/10GB tier).
- Don't market rivals as "broken" on reliability (Deckboard chronic-disconnect claim was refuted).
