# Billy's issue list

Every issue Billy has reported, with its status. Update this on every report and every build.
Status: **open**, **built** (in the game, not yet confirmed by Billy), **confirmed** (Billy saw it work).

## Bodies (Delirium on 47 and NPCs)

| # | Issue | Status |
|---|---|---|
| 1 | 47's Homer doesn't line up with his model (A-pose, clipping into the ground) | built: inverted quaternions + fitted body. Billy: "good step forward, not quite" |
| 2 | 47 shakes, worst when moving while ragdolled | built: movement blocked while down; whole 1/60 steps, pose drawn between the last two (no 60 Hz judder) |
| 3 | 47's head twists a lot when ragdolled | open: neck/head bone map now logged, check next run |
| 4 | 47 stays down after falling / twitches on the ground, must get up | built: learned get-up gets its full 8 s on frozen ground, then blend to his animation; ground no longer sinks |
| 5 | Player can keep moving while ragdolled | built (same as 2) |
| 6 | NPC skeletons not on their bodies; NPCs jump and fly around (Numpad 1) | built: ground rays ignore characters, fitted bodies, keyframed until hit |
| 7 | NPC Homers hover above their bodies, shake violently | built: ground rays ignore characters; NM stepped every frame |
| 8 | NPCs fly into the sky when touched or ragdolled | built: standing NPCs no longer collide with other bodies (nm_agent_collide_agents) |
| 9 | Bumping into an NPC doesn't ragdoll it | built: 100 N·s bump |
| 10 | Female NPCs use Wilma | built |
| 11 | Holding objects or guns while ragdolled | open: plan is to drop the item when he goes down |
| 22 | Ragdolls have no behaviours (fall over limp, no balance / catch fall / high fall) | built: balance on every knock-down (unlimited steps, Playroom default), high fall past 6 m/s, shots keep their own reactions |
| 23 | Knocked-over NPC fell through the map | built: a missed ground ray kept the ground 1 m under the body each update; now keeps the last ground |
| 24 | 47's arms match less while running / walking with the gun | open: the gun's aim layer likely moves the arms after the pose we read |

## Portals

| # | Issue | Status |
|---|---|---|
| 12 | Portals teleport instead of a smooth walk-through like the Playroom; walking in flung him back out | built: ragdoll dive removed (it never crossed); walk-in teleports again; ragdolls still cross by the Playroom transit |
| 13 | Half in, half out of a portal (Playroom ghosts) | open: Hitman's mesh is one piece; needs a second copy or clip planes |
| 14 | Portals aren't see-through (camera window: both black) | built: the texture's tablet now sits inside 47 (it was at z -999, so Glacier never rendered it) |
| 15 | Portals too loud | built: volume slider, 0.3 by default |
| 16 | Portals only stick to perfectly flat surfaces | built: 10 cm / 37 degree tolerance |
| 17 | How to shoot orange when right-click is aim | built: right-click fires orange with the gun out |
| 25 | Portal shoots by itself when walking into a portal | built: only a real right click fires orange; no shots while 47 is down |
| 26 | Game hitches when shooting or entering portals | built: placement casts about half the rays; the ragdoll dive (entering) is gone |
| 27 | 47's Homer drops into the ground after shooting a portal | built (likely the sinking ground, #23) |
| 28 | Portal 2 lets you clip the portal edges when walking in | built: trigger reaches 25 cm past the rim |
| 29 | Square / rectangle portals; still the old October rings | open |

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
