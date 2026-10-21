# Predictive Input Buffer (Geode mod for Geometry Dash)

Pads short clicks so a click made slightly before landing or touching an orb still counts.
Press **F6 in a level** to cycle the stage (**F7** prints session stats):

| Stage | All modes | Wave | At 60 fps |
|-------|-----------|------|-----------|
| 1 (start) | 2 ticks | 2 ticks | 0.5f |
| 2 | 4 ticks | 4 ticks | 1.0f |
| 3 | 8 ticks | 8 ticks | 2.0f |
| 4 | 16 ticks | 8 ticks | 4.0f / 2.0f |
| 5 | OFF | OFF | 100% raw input |

1 tick = 1/240 s, so 4 ticks = 1 frame at 60 fps.

## How it works

Releases of the jump button are held back until the click has lasted at least N ticks.
If you land inside that window while the game still sees the button as held, the jump
registers. Presses are never delayed, only releases.

- **Cube, ball, robot, spider:** only clicks that began airborne are padded (setting "Predictive").
- **Wave:** uses the Wave window (shorter at stage 4). Turn off with "Buffer the wave".
- **Ship, UFO, swing:** raw by default. Turn on "Buffer ship, UFO and swing" to pad them too.
- Dying or restarting clears any held-back release.

Messages appear in the Geode console and as an in-game toast.
Turn on "Log every padded click" to see each padded release with its length in ticks.

## Install (build with GitHub, no local toolchain)

1. Create a new GitHub repository and upload everything in this folder, keeping the structure.
2. Open **Actions > Build Geode Mod > Run workflow**.
3. When it finishes, download the artifact, unzip it, and copy the `.geode` file to
   `Geometry Dash/geode/mods/`.
4. Start the game. Mods installed this way show up in Geode's mod list.

## Build locally

Install the Geode CLI and SDK (see docs.geode-sdk.org), then from this folder:

    geode build

The `.geode` file is written to the `build` folder.

## Before you edit

Change `id` and `developer` in `mod.json` from `yourname` to your own name.
If your GD version is not 2.2081, edit the `gd` block to match it.

## Fair play

This changes the inputs the game receives. For practice or accessibility it is fine, but
check the rules of any leaderboard or list before using it on a record.

## v4.7: external console

1. Turn on **Write external event log** in the mod settings.
2. The Geode console prints the log file path (`events.jsonl`) at startup.
3. In a separate terminal: `python console.py "<that path>"`

It shows per-stage click counts, padded and assisted clicks, a histogram of raw hold lengths, and the latest releases.
