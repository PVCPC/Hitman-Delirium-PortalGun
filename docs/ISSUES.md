# Billy's issue list

Every issue Billy has reported, with its status. Update this on every report and every build.
Status: **open**, **built** (in the game, not yet confirmed by Billy), **confirmed** (Billy saw it work).

## Bodies (Delirium on 47 and NPCs)

| # | Issue | Status |
|---|---|---|
| 1 | 47's Homer doesn't line up with his model (A-pose, clipping into the ground) | built: inverted quaternions + fitted body. Billy: "good step forward, not quite" |
| 2 | 47 shakes, worst when moving while ragdolled | built: movement blocked while down; whole 1/60 steps, pose drawn between the last two (no 60 Hz judder) |
| 3 | 47's head twists a lot when ragdolled | open: neck/head bone map now logged, check next run |
| 4 | 47 gets up with no animation (snaps upright); learned get-up doesn't work on him | partly: blend 1.2 s. Learned get-up runs but ends without standing him (net learned on Homer's proportions). Real fix: Hitman's own get-up clip, open |
| 5 | Player can keep moving while ragdolled | built (same as 2) |
| 6 | NPC models twist and contort around their Homer body | built: bodies fitted on the bind pose, not the walking frame (a bent elbow made a bent-armed body) |
| 7 | NPC Homers hover above their bodies, shake violently | built: ground rays ignore characters; NM stepped every frame |
| 8 | NPCs fly into the sky when touched or ragdolled | built: standing NPCs no longer collide with other bodies (nm_agent_collide_agents) |
| 9 | 47 snags on peds, standing or downed | built: bump from 0.95 m; a downed ped's game capsule is lifted over his head while it lies |
| 10 | Female NPCs use Wilma | built |
| 11 | Holding objects or guns while ragdolled | open: plan is to drop the item when he goes down |
| 22 | Ragdolls have no behaviours (fall over limp, no balance / catch fall / high fall) | built: balance on every knock-down (unlimited steps, Playroom default), high fall past 6 m/s, shots keep their own reactions |
| 23 | Pushed peds fall through the map / walls (still through a shed) | map: confirmed. Walls: 30 cm thick slabs (thin ones were skipped through in one step); wall rays had also been ignoring walls near the pushed ped itself |
| 24 | 47's arms match less while running / walking with the gun | open: the gun's aim layer likely moves the arms after the pose we read |

## Portals

| # | Issue | Status |
|---|---|---|
| 12 | Walking through portals | confirmed: Billy walks through |
| 13 | Half in, half out of a portal (Playroom ghosts) | open: Hitman's mesh is one piece; needs a second copy or clip planes |
| 14 | Portal cameras white / overexposed | built: cameras are picture-in-picture cameras now (the game's tonemapped path) |
| 15 | Portals too loud | built: volume slider, 0.3 by default |
| 16 | Portals only stick to perfectly flat surfaces | built: 10 cm / 37 degree tolerance |
| 17 | How to shoot orange when right-click is aim | built: right-click fires orange with the gun out |
| 25 | Portal shoots by itself when walking into a portal | built: only a real right click fires orange; no shots while 47 is down |
| 26 | Game hitches when shooting or entering portals | built: placement casts about half the rays; the ragdoll dive (entering) is gone |
| 27 | 47's Homer drops into the ground after shooting a portal | built (likely the sinking ground, #23) |
| 28 | Portal 2 lets you clip the portal edges when walking in | built: trigger reaches 25 cm past the rim |
| 29 | Square / rectangle portals; still the old October rings | open |
| 30 | Shooting a portal froze 47 in place | built: the camera tablet sat in his chest and caught him; moved behind the portal |
| 31 | Shooting a ped crashes the game (still, after the first fix) | built: the game's ragdoll is never blocked on NPCs now (its death code walked an empty list, HITMAN3+0x10c3bc4); a ped the game ragdolls is let go by Delirium |
| 32 | Everyone's upper Homer twitches rapidly and violently; nobody's arms line up; peds twist around their Homer | built: UpdateBones runs several passes a frame (base, upper body / aim); we read whichever ran last, which flipped frame to frame. Now always the same (last) pass; the log names each pass's bones |

## Trainer and menu

| # | Issue | Status |
|---|---|---|
| 18 | Menu is terrible | built: own window with Portals and Delirium tabs |
| 19 | "Spawn ped" makes only an outline, not a real Hitman NPC | open: needs the game's NPC template to spawn a real actor |
| 20 | Hitman's own death ragdoll takes over Delirium bodies | built: ActivateRagdoll / ActivatePoweredRagdoll detoured |

## Assets

| # | Issue | Status |
|---|---|---|
| 21 | Portal 2 gun model, animations, cube, effects from Billy's Portal 2 | open |
| 33 | Can't push downed peds any more | built: the new walls were being built out of 47's and the ped's own collision; characters are skipped now |
| 34 | 47's belly sticks out | built: every body's capsules were drawn at Homer's default size, not its fitted size; drawn at its own now |
| 35 | Huge hitches at portals / entering ragdoll | built: ground and wall scans run one body a frame instead of all at once; the log now times anything over 4 ms ([Hitch] lines) |
| 36 | Homer capsules jitter between physics steps | built: drawn interpolated between steps, like the mesh |
