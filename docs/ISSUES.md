# Billy's issue list

Every issue Billy has reported, with its status. Update this on every report and every build.
Status: **open**, **built** (in the game, not yet confirmed by Billy), **confirmed** (Billy saw it work).

## Bodies (Delirium on 47 and NPCs)

| # | Issue | Status |
|---|---|---|
| 1 | 47's Homer doesn't line up with his model (A-pose, clipping into the ground) | built: inverted quaternions + fitted body. Billy: "good step forward, not quite" |
| 2 | 47 shakes, worst when moving while ragdolled | built: movement blocked while down, entity z/turn held |
| 3 | 47's head twists a lot when ragdolled | open: neck/head bone map now logged, check next run |
| 4 | 47 stays down after falling, must get up | built: Delirium's learned get-up (getup.net), then a blend back to his animation |
| 5 | Player can keep moving while ragdolled | built (same as 2) |
| 6 | NPC skeletons not on their bodies; NPCs jump and fly around (Numpad 1) | built: ground rays ignore characters, fitted bodies, keyframed until hit |
| 7 | NPC Homers hover above their bodies, shake violently | built: ground rays no longer hit the NPC's own collider (Pelican) |
| 8 | NPCs fly into the sky when touched or ragdolled | built: standing NPCs no longer collide with other bodies (nm_agent_collide_agents) |
| 9 | Bumping into an NPC doesn't ragdoll it | built: 100 N·s bump |
| 10 | Female NPCs use Wilma | built |
| 11 | Holding objects or guns while ragdolled | open: plan is to drop the item when he goes down |

## Portals

| # | Issue | Status |
|---|---|---|
| 12 | Portals teleport instead of a smooth walk-through like the Playroom | built: the Playroom's ragdoll transit (funnel, whole-body crossing, exit gimme); 47 dives in as a ragdoll |
| 13 | Half in, half out of a portal (Playroom ghosts) | open: Hitman's mesh is one piece; needs a second copy or clip planes |
| 14 | Portals aren't see-through | open: occlusion check off; "Show portal cameras" window needs a screenshot. The Playroom's version also renders too dark |
| 15 | Portals too loud | built: volume slider, 0.3 by default |
| 16 | Portals only stick to perfectly flat surfaces | built: 10 cm / 37 degree tolerance |
| 17 | How to shoot orange when right-click is aim | built: right-click fires orange with the gun out |

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
