# Plan: first-person demo — Jolt physics + raylib rendering (`examples/fps/`)

A third integration example, expanding the sandbox engine's Unity–C# model
into 3D: the host owns a **first-person camera**, a **Jolt Physics** world and
**raylib** rendering; the script owns firing, projectile forking, damage and
enemy behaviour.

## Requirements

1. First person: mouse-look camera attached to the player's capsule.
2. Physics: Jolt (fetched at configure time), raylib renders the state.
3. Left click shoots a projectile — the **host reads the input devices** and
   exposes them through ops; the **spawn logic is in the script**.
4. When a projectile collides, the script checks the projectile component:
   if its remaining `forks > 0`, it spawns 2 projectiles, each aimed at
   another enemy, with `forks` decremented (a chain-lightning-style cascade).
5. Enemies have health; getting shot decrements it. The host renders a
   healthbar billboard above each enemy's head by reading the behaviour's
   script fields back (the same readback pattern as the pickup's `spin`).

## Dependency strategy

- **Jolt**: CMake `FetchContent`, tag `v5.3.0`, built as a static library —
  mirroring how `glscript_link_raylib` falls back to FetchContent. No new
  system packages required.
- **Determinism**: `JobSystemSingleThreaded` + fixed `kDt` → headless runs are
  reproducible for ctest, same contract as the other demos.
- **raylib**: existing `glscript_link_raylib()` (system raylib 6.0 present).

## World & bodies

Arena: flat floor + four walls (static), a few pillars, five enemies in the
open. Body/emotion-type split so contact events fire where the demo needs
them (Jolt raises contact events for dynamic-vs-kinematic/static pairs):

| Kind | Body | Notes |
|---|---|---|
| Player | kinematic capsule | host sets linear velocity from script-driven ops; fixed eye height (no jump) |
| Enemy | kinematic capsule | script chases the player via `set_velocity` |
| Projectile | **dynamic** sphere, gravity factor 0 | straight flight; despawns on contact or lifetime |
| Static | static boxes | floor, walls, pillars |

`Body::SetUserData` carries the entt entity id; a `ContactListener`
implementation only **queues** `(a, b)` pairs on `OnContactAdded` — script
calls never happen inside a Jolt callback. Two object layers
(`NON_MOVING`, `MOVING`) with the usual should-collide matrix.

## Frame order (host)

1. Poll input (windowed: raylib mouse + WASD; headless: scripted timeline).
2. `physics_system.Update(...)`; copy body transforms into the registry.
3. Route queued contact pairs → `on_collision(other)` on both behaviours
   (same routing as the sandbox demo).
4. `update(dt)` on every behaviour.
5. Deferred pass — never re-entering the interpreter:
   - pending spawns: create the entity + Jolt body, attach the behaviour,
     run `on_start`;
   - pending damage intents: `call_method(target, "take_damage", ...)`;
   - pending despawns: destroy the entity **and its Jolt body** (body
     destruction is also deferred).

## Script ↔ host boundary

New host types (registered like the sandbox's `Vec2`):

- `game::Vec3` — fields `x/y/z`, ops `+ - *`, methods `length()`,
  `normalized()`.
- `game::Entity` — methods `position() -> Vec3`, `set_velocity(Vec3)`.

Global effect ops (ambient):

- `app::Input`: `axis("forward"|"strafe"|"fire") -> float`, `look() -> Vec2`
  (mouse delta since last frame). Host owns the devices; the script owns what
  to do with them.
- `app::World`:
  - `player() -> Entity`, `enemies() -> array<Entity>` (the new
    `Runtime::make_array` API),
  - `spawn(prefab: string, pos: Vec3, vel: Vec3, forks: int) -> Entity` —
    per-spawn field override wins over the prefab's (`forks` is the
    projectile's fork budget),
  - `kind(entity) -> string` (prefab kind name — script uses it to ignore
    projectile↔projectile contacts),
  - `camera_forward() -> Vec3` — computed host-side from the player
    behaviour's `yaw`/`pitch` fields (script updates them from `look()`),
  - `despawn(entity)`, `add_kill()`.
- `app::Combat::damage(target: Entity, amount: float)` — an *intent*: the
  host queues it and applies it after script execution finishes (§
  re-entrancy), exactly like `wave.gl`'s effect raises.

## Script (`fps.gl`)

- `PlayerBehaviour`: applies `look()` to `yaw`/`pitch` fields (host reads
  these back for the camera and `camera_forward()`), builds a move vector
  from `axis()`, sets the player's velocity, and on `axis("fire") > 0` with
  cooldown elapsed spawns a projectile at `camera position + forward`,
  velocity `forward * speed` (spawn logic is entirely script-side).
- `ProjectileBehaviour { forks; damage; life; entity }`:
  `on_collision(other)` → despawn self, raise `Combat::damage(other,
  damage)`, and — after checking its own projectile component — if
  `forks > 0`, pick the two nearest enemies in `enemies()` that aren't the
  hit target, aim `(target.pos - my.pos).normalized() * speed`, and spawn two
  children with `forks - 1` each. Contacts with other projectiles are ignored
  via `kind()`.
- `EnemyBehaviour { health; max_health; speed; entity }`: slowly chases the
  player; `take_damage(amount)` decrements `health` and despawns at ≤ 0
  (+ `add_kill()`). The host draws the healthbar from `health`/`max_health`.

## Host-side additions

- `Runtime::make_array(std::vector<Value>)` — public API alongside
  `make_string`/`make_object`, so host ops can return `array<Entity>`.
- Jolt system + listener wiring, body registry, camera handling, billboard
  healthbars (world→screen projection, 2D rect over the 3D frame).

## Headless test (`--headless 400`)

Scripted timeline: turn to face enemy #1 (fixed yaw), fire once at frame N.
Assertions from host-side stats:

- the direct hit damaged enemy #1 (its health dropped by `damage`),
- the fork cascade damaged ≥ 3 *distinct* enemies total (direct + children),
- ≥ 2 enemies died and `add_kill` was counted.

## Out of scope / limitations

- No jumping, no player gravity (kinematic capsule at fixed height).
- No rotation sync from Jolt (enemies render upright; player aims by fields).
- Two-deep fork tree (initial `forks = 2` → 1 → 0), not configurable per hit.
- Healthbars are screen-space rectangles, not world-space quads.
