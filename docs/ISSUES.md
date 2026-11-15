# Billy's issue list

Every issue Billy has reported, with its status. Update this on every report and every build.
Status: **open**, **built** (in the game, not yet confirmed by Billy), **confirmed** (Billy saw it work).

## Bodies (Delirium on 47 and NPCs)

| # | Issue | Status |
|---|---|---|
| 1 | 47's Homer doesn't line up with his model (A-pose, clipping into the ground) | built: inverted quaternions + fitted body. Billy: "good step forward, not quite" |
| 2 | 47 shakes, worst when moving while ragdolled | built: movement blocked while down; whole 1/60 steps, pose drawn between the last two (no 60 Hz judder) |
| 3 | 47's head twists a lot when ragdolled | open: neck/head bone map now logged, check next run |
| 4 | Get-up: snaps upright; learned get-up wrong | Learned get-up removed (Billy). NPCs now get up with Hitman's own recovery: HITMAN3+0x3b82f0(ZActor::m_pAnimatedActor) (what the unconscious/knock-down behaviours call) powers the game ragdoll, picks Impacts|*_GetUp_Back/Stomake/LeftSide/RightSide from the lie (picker 0x1403d9790), plays it, ends at eDE_RecoveryBlendEnd. Byte-checked + __try; falls back to DeactivateRagdoll. Kill-detect ignores the recovery's ragdoll. Untested. |
| 5 | Player can keep moving while ragdolled | built (same as 2) |
| 6 | NPC models twist and contort around their Homer body | built: bind-pose fit; parts baked on the bind pose; only main-chain bones drive parts (twist/roll/cloth/holster/face/extra bones left at rest); one bone-turn rule for all; rig check + map logged per NPC; lying bodies with a part over 5 m/s logged |
| 7 | NPC Homers hover above their bodies, shake violently | built: ground rays ignore characters; NM stepped every frame |
| 8 | NPCs fly into the sky when touched or ragdolled | built: standing NPCs no longer collide with other bodies (nm_agent_collide_agents) |
| 9 | 47 snags on peds, standing or downed | built: bump from 0.95 m; a downed ped's game capsule is lifted over his head while it lies |
| 10 | Female NPCs use Wilma | built |
| 11 | Holding objects or guns while ragdolled | open: the SDK has no drop call; needs reversing the game's drop |
| 22 | Ragdolls have no behaviours (fall over limp, no balance / catch fall / high fall) | built: balance on every knock-down (unlimited steps, Playroom default), high fall past 6 m/s, shots keep their own reactions |
| 23 | Pushed peds fall through the map / walls (still through a shed) | map: confirmed. Walls: 30 cm thick slabs (thin ones were skipped through in one step); wall rays had also been ignoring walls near the pushed ped itself |
| 24 | 47's arms match less while running / walking with the gun | open: the gun's aim layer likely moves the arms after the pose we read |

## Portals

| # | Issue | Status |
|---|---|---|
| 12 | Walking through portals | confirmed: Billy walks through |
| 13 | Half in, half out of a portal (Playroom ghosts) | open: the Playroom draws a second copy of the mesh through the portal with a clip plane at the exit; Hitman needs a second instance of the character's mesh on the same bones |
| 14 | Portal cameras white / overexposed | open again: picture-in-picture only drew the view in the HUD's corner picture (Billy, 11-15); back off. Was: cameras are picture-in-picture cameras now (the game's tonemapped path) |
| 15 | Portals too loud | built: volume slider, 0.3 by default |
| 16 | Portals only stick to perfectly flat surfaces / hard to shoot on walls | built: walls count up to 20 cm bumpy and 53 degrees off; "Easy placement" (on by default) opens a portal where the shot hit even when Portal 2's fit rules say no, if there's wall at its centre |
| 17 | How to shoot orange when right-click is aim | built: right-click fires orange with the gun out |
| 25 | Portal shoots by itself when walking into a portal | built: only a real right click fires orange; no shots while 47 is down |
| 26 | Game hitches when shooting or entering portals | built: placement casts about half the rays; the ragdoll dive (entering) is gone |
| 27 | 47's Homer drops into the ground after shooting a portal | built (likely the sinking ground, #23) |
| 28 | Portal 2 lets you clip the portal edges when walking in | built: trigger reaches 25 cm past the rim |
| 29 | Square / rectangle portals; still the old October rings | built: "Rectangle portals" in the Portals tab; walking, see-through and Delirium bodies (portal-core dp_set_rect, the Playroom's test) all follow it. Playroom rim look needs a shader: not possible with debug lines |
| 30 | Shooting a portal froze 47 in place | built: the camera tablet sat in his chest and caught him; moved behind the portal |
| 31 | Shooting a ped crashes the game (still, after the first fix) | built: the game's ragdoll is never blocked on NPCs now (its death code walked an empty list, HITMAN3+0x10c3bc4); a ped the game ragdolls is let go by Delirium |
| 32 | Everyone's upper Homer twitches rapidly and violently; nobody's arms line up; peds twist around their Homer | built: (Pelican) the body was created in Homer's arms-down pose but the rig baked against the bind pose (arms out): arm parts a hand's length off their bones. Parts now put on the bind pose before the rig; log says how far off they were. Also the same bone pass every frame |

## Trainer and menu

| # | Issue | Status |
|---|---|---|
| 18 | Menu is terrible | built: own window with Portals and Delirium tabs |
| 19 | "Spawn ped" makes only an outline, not a real Hitman NPC | built: "Spawn NPC" in the Delirium tab spawns a real Hitman NPC 3 m ahead (the SDK's npcactor template), dressed as the nearest NPC; Delirium takes it like any NPC (Num1). The old one is "Spawn outline ped" |
| 20 | Hitman's own death ragdoll takes over Delirium bodies | built: ActivateRagdoll / ActivatePoweredRagdoll detoured |

## Assets

| # | Issue | Status |
|---|---|---|
| 21 | Portal 2 gun model, animations, cube, effects from Billy's Portal 2 | open |
| 33 | Can't push downed peds any more | built: the new walls were being built out of 47's and the ped's own collision; characters are skipped now |
| 34 | 47's belly sticks out | built: every body's capsules were drawn at Homer's default size, not its fitted size; drawn at its own now |
| 35 | Huge hitches at portals / entering ragdoll | built: ground and wall scans run one body a frame instead of all at once; the log now times anything over 4 ms ([Hitch] lines) |
| 36 | Homer capsules jitter between physics steps | built: drawn interpolated between steps, like the mesh |
| 37 | Portal rims look like the October debug rings | built: Playroom rims (burning band, tongues, glow, grows open with a flash); "Playroom portal rims" in the Portals tab. Drawn over the screen: a wall in front doesn't hide them yet |
| 38 | NPC bodies take 0.9-2.6 s to make (big hitch); portal cameras 2.1 s at level start | built: the time was reading bone names (a memory check per name: 0.2-2.5 s); cached per rig and checked once per array now. Each on-screen log line cost ~15 ms: Info lines go to the file only. Was measuring: the SDK makes a body in 0.1 ms (timed outside the game), so it's ours or the game's; Attach now logs where its time goes ([Hitch] ...'s body). Bones' log lines never reached PortalGun.log (fixed) |
| 39 | Ground patches 4-9 ms every frame | built: 25 floor + 16 wall rays a body (was 49 + 32) |
| 40 | NPC's Hitman mesh glitches over its (correct) Homer body | built (likely): the character was moved to the body after the pose was written against its old place, so the mesh trailed it a frame; moved first now |
| 41 | Shooting an NPC: Delirium doesn't take over | built: the game still ragdolls it underneath (skipping that crashed) but Delirium keeps the body, shoots the part 47 aims at and drops it dead |
| 42 | Need an invincible 47 and a never-wanted toggle | built: "47 invincible" and "Never wanted (AI ignores 47)" in the Delirium tab (the SDK Player mod's AI cripple pins) |
| 43 | 47's Homer twitches worse | found: the game runs 1 UpdateBones pass a frame (the last-pass capture changed nothing); see #47. Was measuring: the pass report (which UpdateBones pass is captured) now reaches the log; 47's body is only there to bump NPCs ("47's Homer body" / "Show bodies" hide it) |
| 44 | Same bullet push for every gun; automatic weapons only count their first bullet | built: every hit (ZActor_YouGotHit, each bullet of a burst) goes to Delirium as 47's gun from GTA IV's own weaponinfo.xml: handgun = pistol, SMG = mp5, assault rifle = ak47, sniper = sniperrifle, shotgun = shotgun with its 16 pellets fanned; health from 200, dead under 50, the dead still take shots |
| 45 | Corner picture-in-picture still shows the portal view | built: the camera's PIP priority (1 since ab3a424) is 0 now; "Corner picture" checkbox (Portals tab) puts it back if the portal views go dark or white. See-through itself worked this run |
| 46 | Shooting a ped (a kill) leaves it off both its game body and its Homer | built: kills stay Delirium's. The game's death ragdoll still runs (blocking it crashed); found in HITMAN3 (by disassembly) where its bodies live (character +0x68 handler, +0xa8 ragdoll, +0x68 body set) and ZPhysicsObject's SetKinematic (vtable 66). Each body is made kinematic and moved onto its bone on NM's pose every frame, so the mesh follows Delirium. Log: "following its ragdoll's N bodies". |
| 47 | Homer arm capsules held ~35 deg out from 47's arms | built: the bake never ran (nm_build_part_frame returns 0 on success; the check skipped every part), so the rig baked Homer's own arms-down frames against the A-pose bind: exactly its 35 deg. Fixed; the log reads the upper arm's range after the bake (~1.0 right). Was: asked Pelican: the log has joints pinned at 1.00 of their range (clavicles, upper arms, spine3); Hitman's bind pose is arms out ~35 deg, so Fred's limits measured from it likely stop arms hanging down |
| 48 | Game crashed loading a level | fixed (8dbb845): the camera JSON placeholder @PIP@ (5 chars) was swapped as 3, leaving junk; the SDK's Rust QN parser panicked (unwrap on None) and aborted. |
| 49 | Kills still the game's ragdoll (couldn't tell the difference) | built: the ActivateRagdoll hook never installed (MinHook 9: no memory near HITMAN3's code), so kills were never seen. Now seen by polling the game's ragdoll (on with bodies) each frame; Follow then drives it. |
| 50 | 47 ragdolled when shot; wants a toggle, off by default | built: "Delirium on 47" (Delirium tab) now off by default; Delirium is NPCs only unless ticked (Numpad 2 still turns it on for a test). |
| 51 | Replaying the mission crashed | built: ZActor with no animated actor (+0x10B8) in teardown, same as the 01:13 crash: the spawned "Delirium NPC" was never deleted. Spawned NPCs are now deactivated and deleted when the scene clears. |
| 52 | Arms still out (47 screenshot) | FIXED (Billy confirmed): fit on the live pose again (aa518c5); the T-pose bind put hanging arms past the joint limits. |
| 53 | Ragdolled NPCs fly/glitch (shot or pushed: rapid back-forth, up-down) | d6b9401: NPC entity no longer moved/lifted 2.5 m each frame (fought the AI). Next build: ground rays skip the game ragdoll bodies on the NM body; a lying body's patch is no longer rebuilt under it until it moves 0.3 m; no wall panel built through the body (Pelican). Untested. |
| 56 | Pushed/shot NPCs still phase through walls | (Pelican) two-sided contact pushes a part past a slab's midline out the far side: slabs 0.8 m deep behind the hit, 1.2 m+ wide, 12 rays x 3 heights, kept between scans; removed the skip that dropped walls wherever 47 stood near them. Untested. |
| 57 | Arms twitch/jitter a lot; NPCs fall over too easily; re-knocked mid get-up; clip through after; Homer stomach too big; game ragdoll vs Delirium fight | Joints re-centred on the live pose at attach (keyframed arms sat at 1.0-1.9x their range, buzzing at the limits); knock-down only over 2.5 m/s (was 0.6: walking) and never during Hitman's get-up (log: re-bumped 8 ms after get-up, game ragdoll bodies 2-3 m off); DeactivateRagdoll after the game's get-up (character collision back); fitted body weighed 72 kg (58 Wilma), thinning Fred's girth. Untested. |
| 54 | Death: Hitman ragdoll takes over | the game swaps its ragdoll ~1 s after a kill and we let go; now Delirium keeps the body (eaa9bc0). Untested. |
| 55 | Knocked-down NPC: AI walks the game body on, teleport at get-up | game ragdoll activated at knock-down (followed onto NM), deactivated at get-up (eaa9bc0). Untested. |
