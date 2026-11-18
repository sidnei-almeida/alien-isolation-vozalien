# Reverse-engineering notes: how Alien: Isolation hears

Static analysis of the **Steam** `AI.exe` (32-bit, SHA-256 `dde66521798aae8c39ed4e7a3400f561fcecfff83619d1426b9e0cedf5819a90`) done with `objdump`/Python, plus the Brainiac behaviour trees the game ships in `DATA/BEHAVIOR/*.XML`. Every address was verified in `AI.exe` itself. The description of what AIVR does (§7) comes from its own log output and config file, not from its code.

Image base 0x400000; every address below is a VA. At runtime the image is relocated (under Proton it loads at 0x7A040000), so VozAlien uses RVAs = VA − 0x400000. Tags used throughout:
**[C]** = confirmed by reading the disassembly, **[L]** = likely (strong evidence but some inference), **[G]** = guess.

---------------------------------------------------------------------------------------------------
## 0. Summary

The natural hearing path does not use the forced-sense queue (EnqueueSenseEvent 0x4E1C20). It goes through a
global **StimulusManager**, reached as `*(world+0x18)` with `world = *(void**)0x16F0C88`. Anything that makes noise
builds a small "stimulus" object `{type, pos, time, source handle, loudness, flags}` and posts it there. Each
AI character's **SenseProcessor** then pulls new stimuli every frame. For each stimulus it computes a
distance-attenuated raw activation using `max_hearing_distance` and a nav-path or straight-line distance, with a
backstage (vent) penalty. It then pushes the result through the per-listener attribute mapping
(min/max_raw_activation, activation_scalar) into the HEARD_MOVEMENT or HEARD_COMBAT sense accumulator.

**Entry point for the mod [C]:** construct an *extended* stimulus with type 0 using `0x4468B0`, then submit it with
`0x4574D0` on the manager. Footsteps (0x7C71B0) and motion-tracker beeps (0x682D90) are submitted exactly this way.

---------------------------------------------------------------------------------------------------
## 1. Stimulus object

Base object: 0x34 bytes, vtable 0x11C7190. Extended object: 0x3C bytes, vtable 0x11C71B0. **[C]**
(Read from ctors 0x446850/0x4468B0, copy 0x44F6F0, pool clone 0x55EA00→0x5430F0, which allocates 0x34.)

| off | type | meaning |
|---|---|---|
| +0x00 | ptr | vtable |
| +0x04 | u32[2] | per-listener "already processed" bitset (64 listener slots); zeroed by ctor |
| +0x0C | int | **stimulus type 0..6** (see §1.1) |
| +0x10 | u32 | manager frame stamp (written by the manager when the stimulus is added) |
| +0x14 | ptr | next pointer in the per-type list |
| +0x18 | vec3 | world position |
| +0x24 | float | time = `(float)*(double*)0x16410A8` (game clock) |
| +0x28 | u32[2] | source entity handle (a character's handle lives at char+0x54, see 0x77A2F0) |
| +0x30 | float | **loudness** (0..1 in practice) |
| +0x34..+0x39 | u8[6] | flags (extended object only). The type-0 handler reads +0x34/+0x35/+0x36 (§3.2) |
| +0x3A | u8 | extra flag (extended object only; 0x8467D0 passes 1, the other producers pass 0; meaning unknown) |

> A type-0 stimulus **must** be the extended version. The base object is only 0x34 bytes in the pool, so the
> type-0 handler would read heap garbage at +0x34..+0x36. **[C]**

### 1.1 Stimulus types (switch in 0x730C30, jump table 0x731040)
* **0 = movement noise** → HEARD_MOVEMENT (sense 2). Producers:
  * footsteps 0x7C71B0. Loudness comes from the footstep_loudness_* attributes at `[char+0x1154]+0x9E8..0xA00`, in this order: maxed_out_speed, min_speed_considered, min_clamped_input, max_clamped_input, min_scaled_output, max_scaled_output, crouch_multiplier. Rate-limited to one stimulus per 0.25 s per character. Position = char+0x70, flags from (char vfunc 0x1C result)+0x3E38. **[C]**
  * motion-tracker beep 0x682D90. Loudness 0.75 (mode 1) or 1.0 (other modes) (0x163D07C/0x163D080), at most every 0.05 s, flags {1,0,0,0,0,0}. Right after posting it updates the "NoiseTracking" stat. **[C]**
  * 0x460D20/0x460F70: loudness 0.4, at most every 0.2 s, at the character's position. They also set the "pending backstage" bytes, so they are probably vent enter/exit noise. **[L]**
  * 0x8467D0: plays `Play_Motion_Tracker_Blip`, then posts a stimulus at the player with loudness 0.1. Its type comes from global 0x174A7C4 (in .bss, so 0 unless something writes it). **[G]** for its purpose.
* **1 = weapon sound** → HEARD_COMBAT (sense 1). Producers: Weapon_AINotifier 0x578010 (loudness 1.0, or 0.6 when weapon type ==0x1B, at the weapon owner's position); weapon component code 0x662F90 and 0x6632E0 (loudness 0.1, at the target and owner positions); 0x5781A0 (loudness 0.8). Weapon stimuli use the **base** object and are submitted with 0x5772F0. **[C]**
* **2 = "SCANNER" proximity event.** A listener inside a 2D radius (tuning table "SCANNER", +0x11C) gets a forced movement-sense level (mode 2, level 2, activation 0.1). Producer: 0x5781A0 when `[obj+0xD4]==6`. **[C]** for the mechanics, **[G]** for what produces it in gameplay.
* **3, 4, 5**: not handled by the per-NPC listener. Type 4 is bullet impact, loudness 0.25 (0x5781A0); the squad listener (§5) consumes it. Type 5 is produced by 0x6456E0. **[C]**
* **6 = "player was here" hint**, produced by the **Door** CATHODE entity (0x4E30E0, called from door method 0x4E6D20). The handler 0x6E2770 does not touch any sense meter. It writes `pos` and `time` directly into every listening NPC's memory record for the player: `rec+0x3E8` = position, `rec+0x488` = time, `rec+0x497` = 0. **[C]**

---------------------------------------------------------------------------------------------------
## 2. Submit API (all __thiscall, game thread)

```
void* world = *(void**)0x16F0C88;
void* mgr   = *(void**)((char*)world + 0x18);                // StimulusManager
void* playerList = *(void**)((char*)world + 0x8);
Character* player = (*(uint32_t*)((char*)playerList+0x84) > 0) ? **(Character***)((char*)playerList+0x80) : 0;   // [C] same idiom at 0x8467D0, 0x4E3103, 0x730FD9
float  t      = (float)*(double*)0x16410A8;                    // [C]
float* pos    = (float*)((char*)player + 0x70);                // [C]
uint32_t* h   = (uint32_t*)((char*)player + 0x54);             // Character::GetHandle 0x77A2F0 = lea eax,[ecx+0x54]  [C]
```
* `0x4468B0 ExtStimulus::ctor(this, int type, const float* pos, float time, const uint32_t* handle2, float loudness, const uint8_t* flags6, uint8_t extra)`. Callee cleans the stack (ret 0x1C) and returns `this`. Use a 0x3C-byte buffer. **[C]**
* `0x446850 BaseStimulus::ctor(this, int type, const float* pos, float time, const uint32_t* handle2, float loudness)` ret 0x14. Use a 0x34-byte buffer. **[C]**
* `0x4574D0 StimulusManager::AddExtended(mgr, const ExtStimulus*, bool replaceSameType)` ret 8. Copies the object into a pool node (0x3C), stamps it with `[mgr+0x20]`, and appends it to `[mgr+4+type*4]` (0x44A110). The caller's buffer may be thrown away afterwards; the game only resets the vtable. **[C]**
* `0x5772F0 StimulusManager::AddBase(mgr, const BaseStimulus*, bool replaceSameType)` ret 8. Same as above with 0x34 bytes. **[C]**
* `replaceSameType=true` frees **all** queued stimuli of that type first. Every game producer passes 0. **[C]**
* **Lifetime [C]:** `0x7A5310 StimulusManager::Tick()` (__thiscall, no args, prologue `83 EC 0C FF 41 20`) increments `[mgr+0x20]` and frees every stimulus with `stamp_now - stim.stamp >= 2`. It is called from 0x7B41A3 (a virtual per-frame world update) only when `([*0x16F194C]+4) & 0x179FF == 0`, which probably means "not paused" **[L]**. Each stimulus therefore survives about two manager ticks, and every SenseProcessor update in that window sees it exactly once (processed bitset).

**Hook suggestion:** detour 0x7A5310. Call the original first, then post the mic stimulus on the same `ecx`. This
runs on the game thread, once per simulation tick, and only while the AI is running. Rate-limit the posts yourself.
Footsteps post at most every 0.25 s, and accumulation is additive per stimulus (§3.4), so the post rate directly
scales how fast the meter fills.

---------------------------------------------------------------------------------------------------
## 3. Per-listener processing

### 3.1 Dispatch
* Each AI character has a Brain (`0x733850` builds it). `brain+0xF4` = SenseProcessor X (0xC8 bytes, ctor 0x72D7B0).
  `X+0x8C` = character. `X+0x5C` = embedded stimulus listener (vtable 0x12EDF34: [+4] = **0x730C30 OnStimulus**).
  `X+0x70` = listener for the forced-event queue at char+0x3BA8 (vtable 0x12ED284). **[C]**
* Init 0x710550 registers `X+0x5C` with the manager through `0x6EC240 StimulusManager::RegisterListener`, which takes one of 56 slots, stores the slot index at listener+8 and the manager pointer at listener+0xC. The subscription mask is types {0,1,2,6} (`or [edi+4], 1|2|4|0x40`). **[C]**
* Brain::Update 0x7342A0 → `0x7325E0 SenseProcessor::Update` (every AI character, every frame) → `0x6D8FB0 StimulusManager::Dispatch(listener, true)`. Dispatch loops over types 0..6. For each subscribed type, it calls `listener->vfunc[+4](stim)` on every stimulus whose bit for this listener's slot is still clear in `stim+4`, then sets the bit. **[C]**
  There is no spatial pre-filter: every listener evaluates every stimulus.

### 3.2 0x730C30 OnStimulus(stim)
__thiscall, ecx = listener (X+0x5C), `[ecx+0x10]` = X, `[X+0x8C]` = listener character L. **[C]**

Common gates: the source entity S is resolved from the handle at stim+0x28 (`0x6545C0` on registry 0x174EF40) and
must be non-null. `0x77A800(L+0x280, S)` must say S is hostile or relevant to L (faction table at `[world+0x34]`). S != L. **[C]**

Type-0 specific (character class lives at `[[char+0x64]+4]`: 0=PLAYER, 1=ALIEN, 2/7=ANDROID **[L]**):
* skip if `S is PLAYER && flags[2] (+0x36)` **[C]**
* skip if `L is ALIEN && S is PLAYER && flags[0] (+0x34) && L.[0x2DA]`: the backstage Alien does not hear this sound. The motion tracker sets this flag. **[C]**
* skip if `L is ALIEN && L.[0x2DA] && S is not PLAYER`: a backstage Alien ignores non-player movement noise. **[C]**
* `bs = IsBackstage(L) || IsBackstage(S)` (0x77A350). `direct = L is ALIEN && S is PLAYER && flags[1] (+0x35)`.
  * neither `bs` nor `direct` → **nav-path distance** (usePath=1)
  * `bs && !direct` → **straight-line distance × 2.0** (0x13E3D28): sound in or out of a vent carries half as far
  * `direct` → straight-line distance × 1.0  **[C]**
* `raw = 0x6E9F10(&stim.pos, stim.loudness, usePath, S, mult)` (a thunk to 0x6D7CD0), called with ecx = L+0x33E0. If raw > 0, call `0x72F080(S, &pos, 0, -1, stim.time, raw)` on X. **[C]**

Type 1 is the same, except `direct` = `L is ALIEN && S is PLAYER` with no flag needed. The Alien hears player gunfire
by straight-line distance through walls. With backstage involved and not direct: straight-line × 2. Otherwise nav
path. Then `0x72F120(...)`. **[C]**

### 3.3 0x6D7CD0 hearing attenuation
`float __thiscall Hearing::Activation(this=L+0x33E0, const vec3* srcPos, float loudness, bool usePath, Character* src, float mult)`, ret 0x14. `[this+0x128]` = L. **[C]**
* `maxD = 0x775F10 Character::GetMaxHearingDistance(L)`. It switches on the sense set `[L+0x4274]` (0..3) and reads attribute block `[L+0x1154]` at **+0x284 / +0x46C / +0x614 / +0x7BC** (set_1_normal / set_1_heightened / set_2 / set_3). It then adds a per-AI-profile modifier (`[world+0x88]` → 0x6DCC50(`[L+0x42BC]`) → 0x6D2D80 → float), clamps to at least 5.0, and returns 50.0 when the set is >3. **[C]**
  (Note: by the `0x204 + id*4` rule, +0x284 is id 0x20, not 0x1E. The other three sets match ids 0x9A/0x104/0x16E.)
* Distance:
  * `!usePath`: `d = |L.pos(+0x70) − srcPos| * mult` **[C]**
  * `usePath`: both points are raised by 0.5 m and a nav query runs (nav system = 0x8546B0(7); 0x6BF670, then 0x6BF560 with a source character or 0x6BF5E0 without). If the query fails, d = straight-line distance. If the path length is <0 or the first result field is >0.99, d = +inf (inaudible). Otherwise `d = max(straight-line, path length)`. **[L]** for the meaning of the result fields.
* `raw = max(0, loudness − d/maxD)`: loudness 1.0 is audible out to max_hearing_distance and falls off linearly. **[C]**

### 3.4 Sense accumulation
* `0x72F080` / `0x72F120` (__thiscall on X, ret 0x18, args S, pos*, u8 a, int level, float time, float raw) look up or create X's memory record for S (0x6D57F0, ref-counted), then call `rec->0x72EA40` (movement) or `rec->0x72E6D0` (weapon). **[C]**
* `0x6FB2E0 SenseSlot::Scale(raw, senseType)`: `scaled = (min_raw + raw*(max_raw−min_raw)) * activation_scalar` for the current sense set. Accessors: 0x6CD0C0 (min), 0x6CD570 (max), 0x6E16C0 (scalar). For senseType 2 (movement) min/max are at +0x37C/+0x380 (set 1 normal), +0x524/+0x528, +0x6CC/+0x6D0, +0x874/+0x878. For senseType 1 (weapon): +0x30C/+0x310, … **[C]**
* `0x729120 SenseSlot::Add(pos*, raw, scaled, senseType, mode, level, 0, a, time)` (ret 0x24). Movement uses record slot rec+0x100, weapon uses rec+0x8C, damaged rec+0x174; the COMBINED slot at rec+0x344 is fed too when enabled. Mode 0, the natural path, does `act = min(1, act + scaled)`. For each of the 4 thresholds (TRACE/LOWER/ACTIVATED/UPPER, getter 0x6CBFE0) that `act` reaches, it stores the **exact** stimulus position and the time, and posts threshold events to `char+0x3000` (0x713F70 / 0x714100 / 0x71C1D0). The slot is ignored if `[slot+0x72]` is set and the source has `[0x2D9]` or `[0x2DA]` set (source hidden or backstage). **[C]**
* Decay and the positional_accuracy_scalar are **not located**. The stored position is exact, so accuracy is probably applied downstream when a search point is chosen. **[G]**

---------------------------------------------------------------------------------------------------
## 4. Backstage (vents)

* `Character::IsBackstage` **0x77A350** (__thiscall, returns bool): `char[0x2DA] || char[0x2DB]`. The CONDITION_IS_BACKSTAGE BT node calls exactly this (Evaluate 0x6EBA60, GetName 0x6EBA50). **[C]**
  * `0x77A300` returns `char[0x2DA]` (fully backstage). `0x77A340` returns `[0x2DB]`, `0x77A370` returns `[0x2DB] && [0x2DC]` ("entering, committed"). 0x77A310, 0x77A320 and 0x77A330 set or clear `[0x2DB]` and `[0x2DC]`. **[C]**
  * `0x78B140 Character::SetBackstage(bool)` writes `[0x2DA]` and `[0x2D9]`. `0x78B100` writes `[0x2D9]` only. Example: 0x64A6E0, when a pending entry is committed, clears the pending bytes and calls SetBackstage(1). **[C]**
* Hearing effects (all **[C]**, see §3.2): if L or S is backstage, the nav path is replaced by straight-line ×2. Flag +0x34 makes a sound inaudible to a backstage Alien. A backstage Alien ignores non-player movement noise. The accumulator drops sounds whose source is backstage or hidden when `[slot+0x72]` is set.
* `ALIEN_HEARD_BACKSTAGE` is a value of the **DIALOGUE_ACTION** enum. `Alien_Heard_Backstage` is a **DIALOGUE_ARGUMENT** enum value (index 0x21, decoder 0x8B7940). `HEARD_BS_ALIEN` belongs to the human NPC group/branch enum next to ALIEN_SIGHTED. All three are **human NPC reactions to hearing the Alien in the vents**. They are not an input to the Alien's hearing. **[L]** (from the enum string tables and decoders)

---------------------------------------------------------------------------------------------------
## 5. Other listeners and systems
* Squad/group listener, vtable 0x12F18F0, OnStimulus 0x73DFF0, dispatched from 0x743876. Type 1 goes to 0x738910, which runs the same hearing test with straight-line distance ×1 for each group member (stride 0x44) and stamps `rec+0x450` = "heard gunshot" time. Type 4 goes to 0x738B30. Human coordination only. **[C]** for the mechanics, **[L]** for the purpose.
* `[world+0x64]` → 0x6E34C0, called by Weapon_AINotifier: a shooter-to-impact line test, probably bullet near-miss notification. **[G]**
* **Menace manager** (menace_manager.cpp; vtable 0x12B4B00; 0x6CF080 casts rays from `[this+0x1D0]+0x70`; tuning values menace_gauge_seconds_to_fill etc.). None of the 15 stimulus producers above are in this code, so it does **not** route sounds. **[L]**
* **Director-style position hints** that exist: the type-6 "player was here" stimulus (Door), which overwrites each NPC's last-known player position with no sense activation, and NPC_FakeSense (forced). **[C]**
* BackstageAreaSweep / BackstageAlienResponse are BT atomic actions. Factories: 0x6ED630 (vtable 0x12BC8B8) and 0x6F6CA0 (vtable 0x12CA338). Name getters: 0x6C9190 and 0x6D23D0. How they choose a target position was **not traced**, and no position-taking "search here" function was found. **[open]**

---------------------------------------------------------------------------------------------------
## 6. Mod sketch (MSVC, x86)
```cpp
struct alignas(4) Stim { uint8_t raw[0x3C]; };
using ExtCtor_t = void*(__thiscall*)(void*, int, const float*, float, const uint32_t*, float, const uint8_t*, uint8_t);
using AddExt_t  = void (__thiscall*)(void*, const void*, bool);
auto ExtCtor = (ExtCtor_t)0x4468B0;  auto AddExt = (AddExt_t)0x4574D0;
// inside detour of 0x7A5310 (after calling original), on game thread:
Stim s; uint8_t flags[6] = {0,0,0,0,0,0};   // {1,..}: backstage Alien can't hear it (motion-tracker style)
                                           // {0,1,..}: Alien hears it by straight-line distance, no vent penalty
ExtCtor(&s, /*type*/0, pos, t, h, loud /*0..1*/, flags, 0);
AddExt(mgr, &s, false);
```
For a single shout that should hit like a gunshot, use type 1 with a base stimulus (0x446850 + 0x5772F0). It feeds
HEARD_COMBAT instead, and the Alien hears the player's type 1 sounds through walls.

---------------------------------------------------------------------------------------------------
## 7. The forced path: NPC_FakeSense (what the original AIVR mod uses)

* CATHODE entity `NPC_FakeSense` (params `SensedObject`, `FakePosition`, `Sense`, `ForceThreshold`). Handler ~0x4E3540: `switch (SENSORY_TYPE 0..4)` → kinds 9..13 (VISUAL 9, WEAPON_SOUND 10, **MOVEMENT_SOUND 11**, DAMAGE_CAUSED 12, TOUCHED 13). **[C]**
* Event ctor **0x4AB520** (thiscall, ret 0x24), 0x3C-byte object, vtable 0x11E74C8:
  `(kind, handle.lo, handle.hi, const vec3* pos, float 1.0, u8 0, int threshold, float gameTime, float 0.03125)` → fields +0x08 kind, +0x14 pos, +0x20/+0x24 handle, +0x28 1.0, +0x2C time, +0x30 0.03125 (from 0x11F70E4), +0x34 threshold, +0x38 flag. **[C]**
* Delivered with **EnqueueSenseEvent 0x4E1C20** (thiscall on `character + 0x3BA8`, args `(event*, 0)`, ret 8): it goes straight into one character's perception queue. It bypasses distance, hearing range, vents and the sense meter. **[C]**
* Internal threshold enum (name→value code at 0x6C70DE): **TRACE 0, LOWER 1, ACTIVATED 2, UPPER 3**. **[C]**
* AIVR builds exactly this event with kind 11, the player's handle and position, and threshold **3 (UPPER)** for every utterance above its mic threshold, for any Alien distance. That is why it feels like a ping.

---------------------------------------------------------------------------------------------------
## 8. Characters, player and vents (used by VozAlien)

* World root: `*(void**)0x16F0C88`. Character manager `= *(root+0x08)`: list A `[+0x80]`/count `[+0x84]`, list B `[+0xD0]`/count `[+0xD4]` (see getter 0x78A770). **[C]**
* Character class: `*(int*)(*(char**)(chr+0x64) + 4)` → 0 player, 1 Alien (enum CHARACTER_CLASS_COMBINATION: PLAYER_ONLY, ALIEN_ONLY, ANDROID_ONLY, …). **[C]** (read at 0x5192D9)
* Handle: `chr+0x54` (Character::GetHandle 0x77A2F0). Position: `chr+0x70` (vec3). Perception: `chr+0x3BA8`. Backstage: `chr[0x2DA] || chr[0x2DB]` (0x77A350). **[C]**
* Game clock (double, seconds): 0x16410A8. **[C]**

## 9. Hook used by VozAlien

* `StimulusManager::Tick` (0x7A5310) is called from **0x7B41A3** (`E8 rel32`, `ecx = *(root+0x18)`), only when the world is simulating (`[[0x16F194C]+4] & 0x179FF == 0`). VozAlien rewrites that rel32 to its own thiscall function, calls the original first, then posts at most one stimulus per tick. The patch is refused if the call already points somewhere else. **[C]**
* START_NEXT_FRAME (0x774FA0, registered at 0x7D6370) is an alternative per-frame hook; v0.1 used it.

## 10. Behaviour tree notes (DATA/BEHAVIOR, Brainiac XML)

* `ALIEN_BEHAVE.XML` picks a branch by **motivation** (THREAT_AWARE, ATTACK, BREAKOUT, BACKSTAGE_STALK, SUSPECT_TARGET_RESPONSE, SEARCH_SYSTEMATIC, …). Motivations are computed in code from the sense meters.
* `ALIEN_SUSPECT_TARGET_RESPONSE.XML`: fires when any sense (incl. HEARD_MOVEMENT) newly crosses **LOWER_THRESHOLD**, the Alien is **not** in a vent, and a 30 s delay timer expired → `ActionSuspectTargetResponse` (go and check the sensed position).
* In vents the Alien runs `ALIEN_BACKSTAGE_STALK.XML` (`ActionBackstageAreaSweep` BACKSTAGE_ONLY or ALLOW_KILLTRAP), `ALIEN_IN_VENT.XML` and `ALIEN_BREAKOUT.XML` (`SHOULD_BREAKOUT` → `ActionBreakout`). Killtraps, ambushes and withdrawals are all decided there; a noise stimulus only feeds the senses that drive them.
