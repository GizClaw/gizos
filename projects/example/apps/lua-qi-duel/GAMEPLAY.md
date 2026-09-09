# First playable rules and connectivity

## Entry and controls

The default native H106 and AMOLED apps have no mode menu. They wait on a
top-down radial particle field; one screen click (or H106 Record) starts Bluetooth
pairing. The search lasts at most 8000 ms. A connected peer selects online play;
otherwise the app stops discovery and enters a computer match. Desktop's
unsupported BLE/Crypto providers therefore exercise the same eight-second
fallback instead of showing an error page.

During discovery the particle clock accelerates from slow to fast. Once a peer
connects (after a 2400 ms minimum visual lead-in), or once the timeout expires,
it decelerates for 650 ms. The fake-3D camera then interpolates the particle field
from a top-down projection to the normal 45-degree arena projection over 1450 ms.
Only after the tilt finishes do the arena, fighters and HUD fade in over 480 ms.
The first three-second choice round begins after that fade, never underneath it.

In-game mouse controls: left/right rotate one physical position; center locks
the selected action. H106 Volume+/- select skills and Record locks the action.

Each choice window is **3 seconds**, displayed as large pixel digits **3 → 2 → 1** (no decimal/text timer). Choices
remain locked and hidden for the full countdown, even if both players confirm early.
Settlement can only begin at/after the deadline. If not explicitly confirmed,
the currently centered skill is automatically submitted at the deadline (no empty move).
An unavailable automatic skill is still invalid, not replaced with a different skill.

H106 skill sprites are 82% of their previous size. The approved glass texture
is tinted cyan (charge), amber (wave), violet (absorb), and emerald (guard);
unavailable skills use the original gray atlas. Confirmation compresses/rebounds
and sweeps an alpha-masked white sheen across the icon in 250ms, then retains a
brighter locked appearance. Rotation is disabled until the round ends. The growing
release echo only starts at settlement. Tints/sheen are baked using
`app/tools/pack_skill_tints.mjs`; no new renderer API or ring/background is added.

The confirmed countdown artwork is packed from `assets/source/countdown-tricolor-v1.png`
into `assets/generated/countdown.h2r8`: 3 cyan, 2 violet, 1 amber.
H106 shows one 28px transparent tile centered in the previous status-text area.
Round/action/qi/cooldown/result text overlays are removed; the health/qi meters,
disabled skill appearance and animations remain.
Packing: `NODE_PATH=<sharp modules> node app/tools/pack_countdown.mjs
assets/source/countdown-tricolor-v1.png assets/generated/countdown.h2r8`.

## Rules

Both players start at 5 HP, 0/5 qi. Decisions use an immutable round-start
snapshot; the pure `scripts/rules.lua` resolver returns a new state:

- Charge: +1 qi, including if hit; caps at 5.
- Wave: costs 1 qi, 1 damage. At exactly 5 qi, costs all 5 for a three-hit combo.
- Guard: blocks ordinary wave. Combo blocks two hits, takes one damage and
  breaks the shield; guard is unavailable for the following round only.
- Absorb: consumes the first incoming hit, gains 1 qi and avoids that hit.
  Remaining combo hits deal damage. No incoming wave means no absorption.
- Opposing waves cancel hit-for-hit; two combos cancel all three.
- Unavailable wave/guard: gray cracked icon, shakes when confirmed. It locks an
  invalid move, which offers no attack, defense or qi gain. No automatic HP loss.
- HP 0 loses; both 0 draws. After the result entry completes, any H106 control
  key (or a screen press) sends `YOU` right and `WIN/LOSE` left. Existing speed
  lights finish their tracks without spawning replacements, naturally leaving
  OLED black before the top-down pairing-wait particles fade back in.

The bot chooses from its legal actions before player input is received, based
only on its own public qi/cooldown. It does not counter-pick a revealed choice.
Meter debug clicks are disabled during play. `--rehearsal` restores visual
inspection and meter clicks; `--action=...` / fixed-time probes also retain the
previous rehearsal path. This pass does not claim firmware FPS or memory parity.

Results are computed before animation. Each actor plays its own selected pose;
combo VFX have three pulses. Displayed HP/qi updates at the common impact point,
750 ms into the 1800 ms result presentation. The next round cannot consume qi
until that presentation finishes.

Wave-versus-wave rounds cut to an isolated pure-black close-up. The
player's cyan beam enters from the near lower-left with a wide mouth and strong
taper; the opponent's violet beam enters from the far upper-right at a smaller
scale. Equal powers hold the electrical contact core at the midpoint. A combo
against an ordinary wave moves that core continuously toward the weaker source,
bends and shortens the weaker beam, then fractures it into colored shards.
Jagged electric arcs, perpendicular rectangular splashes and deterministic
high-frequency displacement supply the impact vibration. Its final 360 ms uses
authored contraction/spark frames before reaching OLED black; a surviving round
fades the arena back in over 200 ms. No arena, actor, HUD or impact-word layer is
rendered beneath this close-up.

The final round stays on black for settlement. `YOU` enters from the left while
`WIN` or `LOSE` enters independently from the right. Both remain outlined during
the 390 ms slide, fill and light over 190 ms after alignment, then hold above
independent diagonal speed lights. Every streak has its own lane, phase, speed,
length, width, colour and wake strength: electric blue/violet for a win and
warning orange/red for a loss. They keep passing until the next-session input.

A player full-qi attack punches in a large, shaking `COMBO` sprite. If the
player's own guard is broken, that round instead shows `ARMOR BREAK`; opponent
combos do not produce a second label. The two player-centric alerts are
mutually exclusive, with a self guard break taking priority. Both labels use
the countdown's beveled pixel language, a white-hot flash, offset echo and
compact radial shards. Source artwork is `assets/source/combo-impact-v1.png`
and `assets/source/armor-break-impact-v1.png`;
`app/tools/pack_impact_labels.mjs` packs them into
`assets/generated/impact-labels.h2r8`.

## Bluetooth reuse and boundaries

The native bridge reuses BloomSpeaker's engine with an optional non-audio
session callback; defaults preserve BloomSpeaker's behavior. It reuses the
600 ms observation window, mutually claimed adjacent peers, central/peripheral
election, LE Secure Connections and bounded iKCP stream. No microphone/audio
capture starts for Qi Duel. Product beacon magic 0xC7 and development service
0xB0C7 / characteristics 0xB0C8, 0xB0C9 isolate it from BloomSpeaker's namespace.
These identifiers are development-only, not assigned production UUIDs.

The Lua-facing `duel_link` module has pair/stop/state/send/receive/nonce/digest.
Each message is 1..512 bytes preceded by a two-byte little-endian length.
Native TX/RX queues each hold 8 messages. Queue overflow/disconnection fails
closed instead of silently dropping a move. All shared state is queued or atomic;
the BLE worker never enters the Lua VM.

**Current desktop runtime defaults to unsupported BLE and crypto providers.**
It falls back to the computer opponent after the eight-second search; this is not
a two-computer Bluetooth simulator.
A firmware launcher must provide working Runtime BLE, Crypto, SystemEvent,
Queue/Task/Sync and the required task policies. No device was flashed, and no
real two-device pairing/gameplay verification has been performed in this pass.
Management-advertising pause/resume support from the paired engine must be
supplied by firmware products that already occupy the advertising slot.

## Game wire protocol v1

`link_protocol.lua` is transport-independent and tested using two independent
monotonic clocks and a reliable in-memory wire. Both roles maintain canonical
central-first state; rendering remaps it to local-player-first.

1. `HELLO/PONG/SYNC`: random 128-bit session ID and NTP-style clock offset.
2. Both `READY`: central sends `START` with a future shared start timestamp and
   the fixed 3000 ms choice window. Each round has a monotonically increasing ID.
3. `COMMIT`: digest of protocol domain, session, round, side, action and a fresh
   random 128-bit nonce. The native digest uses domain-separated HKDF-SHA256
   through Crypto PAL, output 32 bytes. No ad-hoc noncryptographic hash in use.
4. Countdown expired AND both commits received: exchange `REVEAL` and verify against the locked digest.
   A local missed confirmation commits the current candidate. A missing remote packet is NOT an automatic move.
5. Both independently resolve and exchange a canonical `RESULT` digest.
6. Only matching results permit a central `PLAY` timestamp. Both present the
   agreed result, then exchange readiness for the next round.

The 1500 ms post-deadline window is only for delivery/verification, not extra
player decision time. Missing commits/reveals/results pause and end the attempt
without unilateral damage. This first version has no reconnect/resume; re-pair
starts a fresh match. Stale round packets cannot settle a round twice. Malformed
packets, changed commits, bad reveals and result mismatches fail closed.

BloomSpeaker's advertised-value-derived passkey prevents accidental cross-pairs
but does not authenticate against an active nearby MITM. Commit/reveal prevents
reactive move changes by an honest application peer; it is not an anti-cheat
proof of a remote client's clock or firmware. Production pairing needs numeric
comparison/OOB or provisioned secrets, as already documented by BloomSpeaker.

## Verification

- Pure rules tests cover the matchup table, caps, cooldown recovery, 3000 ms
  boundary, automatic candidates, simultaneous cancellation, symmetry and immutability.
- Two-peer protocol simulation covers clock offset, early locking, invalid move,
  next-round automatic candidates, bad reveal and missing-peer timeout without settlement.
- Native UI tests cover the click-to-pair intro, deterministic combat probes and
  existing gesture/render regressions.
- The original browser preview remains an archived art reference, not the game.
