# stillalive c

<p align="center">
  <img src="res/ss/ss.png" alt="Screenshot 1" width="900" style="border-radius:26px;"/>
</p>

A pure C terminal recreation of the Portal end credits sequence.

it runs inside standard terminal emulators, reproducing the aperture science terminal aesthetic with character typing, scrolling credits, and ascii art synchronized to the track.

* responsive layout that dynamically scales to fill the window while preserving the authentic 4:3 portal aspect ratio
* clean terminal handling with alternate screen buffer and artifact free resizing
* lightweight audio playback with automatic fallback across pw play, play, ffplay, mpv, and aplay
* interactive controls to pause, resume, restart, and cycle scaling modes on the fly

## credits and inspiration

* original song and portal game by valve corporation, written by jonathan coulton, performed by ellen mclain
* inspired by the web recreation still alive web by sd skykloud

## requirements

* gcc or clang
* make
* **Linux / BSD / macOS**: any installed audio player (pw-play, play, ffplay, mpv, or aplay)
* **Windows**: Windows 10/11 with Windows Terminal, PowerShell, or Command Prompt (audio is handled natively out of the box via Windows Multimedia)

the audio tracks are included inside the res/song directory.

## building

compile with make:

```bash
make
```

to clean:

```bash
make clean
```

## usage

run the binary directly:

```bash
./stillalive
```

or via make:

```bash
make run
```

### options

* `./stillalive autoplay` : skips the boot screen and starts immediately
* `./stillalive silent` : runs the terminal animation without audio playback
* `./stillalive start=25000` : jumps to a specific timestamp in milliseconds
* `./stillalive aspect` : fits the terminal window using authentic 4:3 proportions (default)
* `./stillalive fill` : expands the boxes to fill the entire window dimensions
* `./stillalive classic` : uses the fixed 98x38 character box

### controls

* `space` : pause or resume
* `s` : cycle scaling mode between aspect fit, full stretch, and classic fixed
* `r` : restart playback from the beginning
* `q` or `esc` : quit and restore terminal state
