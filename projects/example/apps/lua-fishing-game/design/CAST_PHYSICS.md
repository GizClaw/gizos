# Desktop cast and line simulation

## Interaction and geometry

A sea click calls the same press/release gesture path as manual interaction and
starts one complete cast. Further clicks during casting/flight are ignored. A
click after water entry performs one retrieval stroke. Gear changes reset the simulation.
The rod pivots around a point 0.25 m behind the grip, along the grip extension.
Rodrigues rotation about an oblique axis gives the requested circular sweep; a
pinhole camera projects it into the screen. Distributed elastic curvature is
applied to the rod tangents. Waiting moves the handle angle to -2.62 radians,
lowering the tip toward the water instead of restoring the high ready pose.

Coordinates are metres with Y up and Z away from the camera. The eye is 1.6 m
above the water, focal length 260 pixels, horizon y=203. Line particles, lure,
rod and splash share that camera. Catalog lure lengths set their projected size;
a small readability multiplier preserves the procedural pixel-art silhouette.
Near-plane and screen clipping protect native line drawing.

## Dynamics

- Elastic rod: damped reduced-order beam, fixed 120 Hz, length/power/action
  dependent frequency and stiffness; line tension contributes to bending.
- Lure: mass from catalog (explicit estimates where unavailable), gravity
  9.81 m/s² and quadratic aerodynamic drag. Its release velocity comes from
  the constrained moving rig, not an assigned screen-space trajectory.
- Line: 240 Hz Verlet prediction and 12 alternating XPBD distance-constraint
  iterations. Compliance scales by dt². An end-to-end unilateral constraint
  improves convergence across the large lure/line mass ratio without pushing
  a slack line. Existing line spans do not expand when paying out: short spans
  are inserted at the rod tip. A small payout surplus allows slack.
- Reel/line resistance opposes travel. Fly line has greater linear mass and
  repeated strokes with turnover pauses before final delivery. ISO uses a
  separate pendulum gesture.
- Water: swept endpoint intersection, dissipative line contact, a submerged
  lure shadow (topwater and A-WA remain visible), projected expanding ripples
  and short ballistic splash droplets.

Meshes for the resting rod are cached; physics substeps calculate only the tip.
The full projected rod and pixel art are drawn once per rendered frame.

## References and limits

The code is an original Lua implementation informed by:

- [Shimano casting tutorial](https://fish.shimano.com/ja-JP/content/beginners/fishingstyle/lurefishing/action/index.html): casting stages and releasing line.
- [Shimano action and power](https://fish.shimano.com/en-GB/content/c/rod-action-and-power-explained.html): distinction between power and action.
- [Macklin et al., XPBD (2016)](https://matthias-research.github.io/pages/publications/XPBD.pdf): compliant constraints, timestep-scaled compliance and accumulated multipliers.
- [PositionBasedDynamics XPBD implementation](https://github.com/InteractiveComputerGraphics/PositionBasedDynamics/blob/master/PositionBasedDynamics/XPBD.cpp): reference distance-constraint formulation.

This is a physically motivated game model, not a measured reconstruction of
each commercial blank and reel. EI, air drag, line density, payout resistance,
stroke timing and compliance are calibrated parameters. It omits full fluid
dynamics, spool backlash, line self-contact and line wrapping around guides.
Fly-line turnover is simulated by the same particle chain rather than a drawn
infinity curve. No external library or artwork is loaded at runtime.

## Verification

The in-VM `--check` covers all 11 rods reaching water in front of the boat;
repeated casts; finite particle states; depth-dependent scale; lowered waiting
pose; and equal landing distance when driven at 30/60/120 Hz. Existing rod
elasticity, 2,376 pose, gear compatibility and gesture checks are retained.
Default BANTAM/ANTARES/99F reached about 15.4 m; these distances are simulation
results, not manufacturer performance claims.

`cast-demo` provides exact-time native SDL captures. `cast-record` exports a
60 Hz sequence from that same renderer; it is an offline visual check, not an
FPS benchmark. Live `physics-demo` logs FPS and p95 frame interval.

## Water entry and gradual line tightening

The endpoint remains a dynamic mass after water entry. Net vertical force is
`g * (rho_water * volume - mass)`; water drag is quadratic in speed, with
backward damping to keep the fixed-step integrator stable. Estimated volume
respects floating/suspending/sinking catalog types; drag area depends on lure
size and shape. For equal shape and buoyancy class, greater mass reaches a
higher sinking terminal speed. Water density is 1025 kg/m³.

Twelve percent of entry velocity is retained for non-topwater rigs; the rest
is dissipated at impact. Sinking lures pull buoyant line spans below the water
through the same constraints. Paid-out length stays fixed after landing.
Slack decreases through tension, not an animation blending toward a straight
line. Topwater and A-WA remain at the water surface; floating minnows recover
toward it, and suspending lures retain near-neutral buoyancy.

This distinction matters: a heavier hollow lure can still float. The catalog
99F is a floating model, as documented by
[Shimano](https://fish.shimano.com/pt-BR/product/lures/hardlures/a155f00000ccofiqab_p.html).
[Rapala's buoyancy adjustment weights](https://www.rapala.com/us_en/suspendots)
also distinguish floating, suspending and slow-sinking behavior. Volumes,
drag coefficients and resulting sink speeds here remain simulation estimates.

The water regression compares equal-shape 13 g / 26 g sinking models, checks
floating/suspending classification, verifies zero topwater submersion, and
advances a full SETUPPER 26 g cast for 40 seconds after landing. Observed depth
went from 0.230 m at 2 seconds to 4.421 m at 40 seconds; polyline excess length
over its endpoint chord fell from 0.382 m to 0.020 m without further payout.


## Click retrieval

Hard lures and paddle tails retrieve 0.75 m per click, flies 0.50 m, in a 0.58 s smooth stroke. A bounded handle-angle excursion feeds the existing damped beam model; rod length, power/action, and solved line tension determine flex and recovery. No random visual shake or endpoint interpolation is used. At most three pending strokes are queued so rapid clicks cannot cause instantaneous velocity spikes.

A-WA single clicks wind 0.12 m with a smaller handle stroke. Four clicks separated by at most 0.65 s enable sustained 0.55 m strokes until retrieval completes. These are desktop interaction/calibration parameters, not manufacturer specifications.

Material is removed at the guide, consuming spans and deleting their particles without relocating downstream particles. A tiny remaining guide span is merged with its neighbour. The existing 240 Hz XPBD solver transmits pull through slack and water drag; the lure can rise out of water when tension lifts it. Gravity resumes above the surface. The stop retains 0.65 m of leader, or 0.85 m for A-WA. The next click starts a new cast; queued retrieval strokes never trigger casting. A vertical casting gesture also remains available.

Elastic-response reference: [Müller's XPBD pendulum example](https://matthias-research.github.io/pages/challenges/pendulum.html), using compliance and damping for stable physical oscillation. The existing Lua reduced beam and rope solver remain original implementations.

`--scene=retrieve-demo` replays cast plus repeated retrieval actions for native SDL performance/visual checks. Fixed-time captures use the same physical substeps. Regression checks cover hard bait at 30/120 Hz, paddle tail, fly, A-WA burst, sinking bait, one-click travel, leader stop, no accidental recast, finite states, and material-length conservation.


## Tension-only line revision

Local span multipliers are clamped to tension: slack spans no longer generate compression forces. Axial rigidity is calibrated to 8000 N for conventional line and 1200 N for fly line, reducing elastic stretch. Payout targets the predicted next-substep endpoint distance plus a 2.5 cm allowance (8 cm for fly), rather than adding radial speed and an accumulated slack correction. Prediction preserves free spool flight while limiting surplus line.

After release, relative internal-node velocity smoothing damps short-wavelength chatter while retaining bulk motion and larger fly loops. Mass-weighted separating-velocity damping is applied only to taut spans. These are simulation tuning values, not measured properties of a selected spool material. Rod compliance is unchanged. Completed retrieval waits for a fresh click to cast again.

## A-WA float rig

The waiting/casting rope endpoint represents the float. A separate 0.7 m leader places the hook and bait below it, limited by the seabed. Encounter habitat depth and fish approach targets use that underwater hook; ISO rigs never trigger a surface-lure attack. Pecking gives a small float dip; a committed take submerges the float.

On hook-set the rope endpoint transfers to the underwater hook, accounting for leader length before tensioning. The float then lies on the loaded line one leader length from the fish mouth; on release the endpoint transfers back to the float. Its eight-unit icon has an independent perspective scale and minimum readable orange cap. The underwater bait is a muted small mark, never a second orange lure. Runtime remains Lua geometry.
