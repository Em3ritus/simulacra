# Contributing

**Issues are welcome.** Bug reports, questions, and feature requests — open an issue. That includes
build/flash problems, console behavior that doesn't match the [wiki](https://github.com/Em3ritus/simulacra/wiki)
or [README](README.md), and hardware you'd like to see supported.

**Pull requests aren't being merged right now.** Simulacra is currently
**unlicensed — all rights reserved** (see [License](README.md#license)), and until that's settled,
accepting outside code raises questions about whose work it even is. Feel free to fork and hack on
it — just don't expect a PR to land upstream yet. This will change once licensing is sorted; if you
want to know when, watch the repo or check back.

## Filing a good bug report

Include:
- Which board(s) and how they're wired/flashed (browser web-flasher vs. built from source; which
  build flags if from source)
- What you expected vs. what happened
- Serial output around the problem, if you have it (`idf.py monitor` or the flasher's own log)

## Security-relevant findings

This is a privacy tool that transmits RF traffic and handles cryptographic keys (fleet transport key,
CONTROL signing key). If you find something that undermines those specifically — not general bugs,
but something that leaks identity, forges a signed command, or breaks a stated security guarantee in
the [Security model](README.md#security-model) — please still just open an issue. There's no formal
disclosure program; flag it clearly in the title so it doesn't get missed.
