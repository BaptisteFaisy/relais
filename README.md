# relais

Relais — bounded-latency failover between two QNX 8.0 computers steering the same actuator. The actuator is the arbiter and the clock: it hands out mandate numbers, ticks every period, and never applies the order of a deposed machine. QNX Global Hackathon 2026, Automotive & Mobility.

## Pipeline

File de merge Mergify (rebase, historique linéaire) + balayeur `pr-auto-merge` :
checks requis `ci`, `ci-budget` (jobs ≤ 5 min), `couche0` (garde `## Scope`).

- Runners self-hosted r630 : `r630-relais-01..NN` (label `__WORKER__`).
- Toute PR porte une section `## Scope` (puces `- `fichier``) listant ses
  fichiers ; les workflows sont des fichiers de garde (`SAMVISION_ALLOW_GUARD_BUMP`).
- Conflits : agent Fireworks ; tests rouges : agent zeroclaw `fw` via `triage-rouge`.
- `main` est protégée (ruleset) : tout passe par PR + file, jamais de push direct.
