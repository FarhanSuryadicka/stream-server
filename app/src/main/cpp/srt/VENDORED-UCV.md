# Vendored SRT

- Upstream: `https://github.com/Haivision/srt`
- Version: `v1.5.6`
- Commit: `c63c311e88aa55e430e3b7d94b89d790994f88c4`
- License: MPL-2.0 (see `LICENSE` and `COPYING`)

The directory is intentionally stored as ordinary vendored source, without a
nested `.git` directory. The experiment currently builds the static library
with applications, tests, logging, shared-library output, and encryption off.
Encryption will be a separately labelled experiment profile rather than an
implicit build difference.
