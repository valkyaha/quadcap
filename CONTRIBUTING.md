# Contributing to quadcap

Thanks for your interest. This project talks to real hardware through a reverse-engineered driver,
which shapes how it is built and reviewed: claims about behaviour need measurements behind them, and
a change that cannot be verified is hard to accept however reasonable it looks.

## Before you start

For anything beyond a small fix, open an issue first. It is worth agreeing on the approach before
you spend an evening on it, particularly for pipeline changes, where the interaction between
GStreamer negotiation, the driver and the GPU is not always obvious from the code.

Good first issues are labelled [`good first issue`](https://github.com/valkyaha/quadcap/labels/good%20first%20issue).

## Development setup

```bash
git clone https://github.com/valkyaha/quadcap.git
cd quadcap
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite runs without a capture card. Pipeline tests use `videotestsrc` and `audiotestsrc`, so
the full graph — including the segment ring, the recording branch and the three-track audio layout —
is exercised on any machine. Please keep it that way: a test that needs hardware is a test almost
nobody can run.

## Reporting a bug

Include:

- `quadcapd --setup` and `quadcapd --status` output
- The card model, kernel version and distribution
- `cat /proc/sc0710-state` if the card is detected
- For capture problems, what `ffprobe` says about the resulting file

If the picture is wrong rather than absent, an extracted frame is worth a great deal:

```bash
ffmpeg -ss 5 -i recording.mkv -frames:v 1 frame.png
```

Several bugs in this project produced structurally perfect files — right geometry, steady frame rate,
no dropped frames — that were entirely black or sheared. Only looking at a frame revealed them.

## Coding standards

**C++20, four-space indentation, no tabs.** Match the surrounding style rather than introducing
your own; the code is deliberately consistent.

- Prefer `[[nodiscard]]` on anything returning a value a caller should not ignore
- Keep GStreamer refcounting explicit and paired; `gst_clear_object` for members
- Qt types at API boundaries, standard library internally where it is a better fit
- Lines wrap at 100 columns

**Comments should explain why, not what.** The reader can see what the code does. What they cannot
see is that `splitmuxsink` opens the next fragment before flushing the current one, or that
`VIDIOC_DQEVENT` returns `ENOENT` for an empty queue. Those are the comments worth writing. Leave
out anything about the history of the change itself — that belongs in the commit message.

**Measurements over adjectives.** If you claim something is faster, say how much faster and how you
measured it.

## Tests

Every behavioural change needs a test. Bug fixes need a test that fails before the fix.

- **Unit tests** for pure logic: mode parsing, ring-buffer selection, setup diagnosis
- **Integration tests** for anything touching a GStreamer graph, using the test sources

Name tests after the behaviour, not the function: `distrustsTheUnsettledTailWhenScanningBlind` says
what is guaranteed; `testFinalizedSegments` does not.

## Commits and pull requests

- One logical change per commit
- Imperative subject under 72 characters: `Fix EDID readback on MK.2 boards`
- Explain *why* in the body when it is not obvious, and include measurements where relevant
- Rebase on `main` rather than merging it in
- Reference the issue: `Fixes #42`

Pull requests should describe what changed, how you verified it, and on what hardware. "Tested on a
4K60 Pro MK.2 at 1080p60, three audio tracks present, no dropped frames over 20 minutes" tells a
reviewer far more than "works for me".

CI must pass. If a test is flaky, say so rather than re-running until it goes green.

## Working on hardware behaviour

The driver is out of tree and reverse-engineered, so its behaviour is discovered rather than
documented. If you establish something about it, write it down where the next person will find it:
in the code comment next to the workaround, or in the README's troubleshooting section.

Be specific about the board. The MK.2, the 4K Pro and the Cam Link Pro share a PCI ID and differ in
meaningful ways — several driver paths are guarded by board type, and a fix for one can be a
regression for another.

## Licence

Contributions are accepted under the [Apache License 2.0](LICENSE). By submitting a pull request you
confirm you have the right to license your work under those terms.
