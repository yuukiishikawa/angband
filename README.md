# Angband 4.2.6

<p align="center">
  <img src="screenshots/title.png" width="425"/>
  <img src="screenshots/game.png" width="425"/>
</p>

Angband is a graphical dungeon adventure game that uses textual characters to
represent the walls and floors of a dungeon and the inhabitants therein, in the
vein of games like NetHack and Rogue. If you need help in-game, press `?`.

- **Installing Angband:** See the [Official Website](https://angband.github.io/angband/) or [compile it yourself](https://angband.readthedocs.io/en/latest/hacking/compiling.html).
- **How to Play:** [The Angband Manual](https://angband.readthedocs.io/en/latest/)
- **Getting Help:** [Angband Forums](https://angband.live/forums/)

## Borg (Automatic Player)

The Borg is an AI that plays Angband automatically. Press `Ctrl-Z` in-game
to access the Borg command interface (requires `ALLOW_BORG` at compile time).
See [docs/hacking/borg.rst](docs/hacking/borg.rst) for full documentation.

### Headless Mode

Run the Borg without a terminal UI:

```bash
angband -mborg -n [-uName]
```

### Remote Mode (TS Angband)

The Borg can also play against the TypeScript reimplementation of Angband
([angband-ts](../angband-ts/)). Start the TS remote server, then connect:

```bash
# Terminal 1: Start TS Angband remote server
cd ../angband-ts
npx tsx packages/@angband/core/src/borg/remote-server.ts --port 9876

# Terminal 2: Connect C Borg to TS Angband
angband -mborg -n -- --remote localhost:9876
```

This allows the same C Borg AI to test both the original C engine and the
TS engine, enabling cross-implementation verification.

Enjoy!

-- The Angband Dev Team
