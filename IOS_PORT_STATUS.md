# iOS Port Status — Geometry Dash 2.2081

## Target lock

This branch is for **Geometry Dash iOS 2.2081**. The `mod.json` target is:

- `gd.ios = 2.2081`
- `gd.win = 2.2081` (unchanged)

It must not be changed to iOS 2.2091. The iOS build workflow checks the metadata before compiling and stops if the target drifts.

## Hook audit against Geode's 2.2081 bindings

Reviewed the official `bindings/2.2081/GeometryDash.bro` file at:

https://github.com/geode-sdk/bindings/blob/5baf932c04ef25fd38932375081085b104164dcd/bindings/2.2081/GeometryDash.bro

The source-level hook signatures used in this mod match the 2.2081 declarations, and the declarations have iOS addresses in that version:

| Hook or called input method | iOS binding offset |
|---|---:|
| `GJBaseGameLayer::handleButton(bool, int, bool)` | `0x1fabbc` |
| `GJBaseGameLayer::processCommands(float, bool, bool)` | `0x1ff4f8` |
| `PlayerObject::pushButton(PlayerButton)` | `0x227eac` |
| `PlayerObject::releaseButton(PlayerButton)` | `0x228d08` |
| `PauseLayer::customSetup()` | `0x141df8` |
| `PlayLayer::postUpdate(float)` | `0x11f64c` |
| `PlayLayer::destroyPlayer(PlayerObject*, GameObject*)` | `0x11e998` |
| `PlayLayer::resetLevel()` | `0x121b78` |
| `PlayLayer::resetLevelFromStart()` | `0x121ae8` |
| `PlayLayer::loadFromCheckpoint(CheckpointObject*)` | `0x120fc8` |
| `PlayLayer::setupHasCompleted()` | `0x1168c4` |
| `PlayLayer::onExit()` | `0x122c0c` |
| `PlayLayer::getCurrentPercent()` | `0x11e338` |

These are reference offsets from the pinned **2.2081 binding file**, not offsets copied from 2.2091. The code uses Geode `$modify` classes rather than inventing custom addresses. Geode's changelog states that `Modify` classes work on JIT-less iOS; custom raw hooks would need separate review.

## Why the earlier build configuration was wrong

The first iOS metadata used `gd.ios = 2.208`. That selected a different binding version than the one requested. In that older binding, `GJBaseGameLayer::processCommands` does not expose the three-argument iOS signature used by this code. This branch now targets `2.2081` on iOS, matching the source signatures.

## What is and is not verified

- **Verified:** the selected target and the primary hook signatures/entries in the official 2.2081 bindings.
- **Build status:** must be confirmed by the iOS GitHub Actions run after the target correction. Do not infer a successful build from this audit file.
- **Device runtime:** not yet verified. A green build is necessary, but it does not prove the mod behaves correctly or avoids crashes on a real iPhone.

Do not treat this branch as a stable iOS release until both the build and runtime checks pass.
