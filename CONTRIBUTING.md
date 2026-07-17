# Contributing

Thanks for your interest in contributing! This is a small, hobbyist-maintained project, so a bit of
process up front helps keep things manageable for everyone.

## Before you start

- **Check existing issues and PRs first** — search open and recently closed items to avoid
  duplicating work already in progress.
- **Open an issue before a substantial PR** — for anything beyond a small fix or doc change,
  raising an issue first lets the approach get agreed before you put time into it.
- **Keep PRs small and focused** — one change per PR. Bundling unrelated fixes together makes
  review slower and harder to merge safely.

## Build variants

This firmware has five build targets (Bandscope, Broadcast, Basic, RescueOps, Game — see the
README's Main Features section). If your change is variant-specific, say so in the PR description
and mention which variant(s) you built and tested.

## Building

See the README's [Compiler](README.md#compiler) and [Building](README.md#building) sections for
the three supported build methods (GitHub Codespace, Docker, Windows). Please build successfully
before opening a PR — note that CI (`.github/workflows`) currently only builds on push to `main`,
not on pull requests, so a passing local build is the only check your PR will get before review.

## Testing on real hardware

This is firmware for a physical radio (Quansheng UV-K5/K6/5R). Wherever practical, please test your
change on real hardware before submitting, and say what you tested (device, build variant, what you
checked) in the PR description. If your change can't reasonably be tested without hardware you don't
have, say so explicitly rather than leaving it unstated — reviewers can then judge the risk
accordingly.

Per the README's own warning: flashing custom firmware carries a real risk of bricking a radio.
Back up your EEPROM (e.g. with [k5prog](https://github.com/sq5bpf/k5prog)) before testing on your
own hardware.

## Code style

There's no `.clang-format` or formal style guide in this repository yet. Please match the
formatting conventions of the surrounding code in whatever file you're editing.

## Commit messages

Describe *why* a change is needed, not just what changed — especially for anything touching radio
behaviour, timing, or hardware-specific code, where the reasoning may not be obvious from the diff
alone.

## AI-assisted contributions

If you used an AI assistant to help draft a contribution, that's fine, but two things are expected:

1. **Disclose it** in the PR description (e.g. "Drafted with GitHub Copilot, reviewed and tested
   manually").
2. **Understand and be able to explain your own submission.** You must be able to answer questions
   about how your code works and why it's correct, not just that a tool produced it. This applies
   doubly here given the firmware runs on RF-transmitting hardware — an unreviewed change can have
   real consequences beyond a failed build.

## Licence

By contributing, you agree that your contributions will be licensed under this project's
[Apache-2.0 licence](LICENSE).
