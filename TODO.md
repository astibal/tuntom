# Project TODO

## Statistics write strategy

The text statistics export is approaching 100 fields and currently rewrites an
atomic temporary file every second for every tuntom process. The file does not
grow over time, but frequent create/write/rename operations will not scale to a
large number of tunnels.

Deferred design:

- add `--stats-interval <seconds>`, probably with a 10-second default;
- keep the complete periodic dump in one file rather than splitting its schema;
- immediately publish stats on switch connect, disconnect and socket error;
- retain the immediate `SIGUSR2` snapshot;
- retain `--no-stats` with no periodic writes;
- verify whether `SIGUSR1` should restore the configured interval unchanged.

The intended result is current switch diagnostics with roughly one tenth of the
normal filesystem traffic. Revisit before deploying large tunnel populations or
adding substantially more metrics.
