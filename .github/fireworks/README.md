# Agents Fireworks

Deux agents, deux problèmes distincts, jamais une porte de merge : seule la CI
verte fait merger. Tous deux sont lus sur `origin/main` par le sweep (la
branche d'une PR peut être antérieure), et n'écrivent que du texte ou des puces
vérifiées.

## Agent de conflit (`conflict_agent.py`)

Quand le sweep de `pr-auto-merge` échoue à rebaser une PR, il appelle
`conflict_agent.py` dans son clone, rebase en cours. L'agent demande au modèle
Fireworks la résolution de chaque fichier en conflit, termine le rebase, pousse
sur la branche de la PR avec bail et rend compte en commentaire. La file reprend
au passage suivant ; seule la CI verte fait merger.

Les journaux en ajout pur (`CHANGELOG.md`, `CHANGELOG`) ne passent jamais par le
modèle : deux entrées écrites en tête de fichier ne sont pas un désaccord, elles
sont réunies sans perte (`union_journal.py`, `union_sans_perte`). Un bloc qui
remplacerait une ligne commune, lui, reste au modèle. C'est ce qui a arrêté la
boucle du 29/10 (#215 rebasée six fois sur `CHANGELOG.md`, une saisine et un
quota par rebase). Les empreintes et plafonds vivent dans `quota.py`.

Les deux modules sont chargés par `conflict_agent.py` depuis `origin/main`
(import direct d'abord) : le sweep ne copie que le script d'agent dans `/tmp`,
et une branche de PR peut être antérieure à ce découpage.

## Contrat d'appel

```bash
FIREWORKS_API_KEY=... python3 conflict_agent.py resolve \
  --repo BaptisteFaisy/relais --pr 118 --branch feat/x --sha <tête>
```

Le script est lu sur `origin/main` (pas sur la branche de la PR, qui peut
être antérieure). Sorties : `0` poussé, `1` échec avec compte rendu,
`2` rien à faire (l'appelant saute sans bruit).

## Réglages

| Variable / secret | Défaut | Effet |
|---|---|---|
| secret `FIREWORKS_API_KEY` | — | sans lui, l'agent ne fait rien |
| var `FIREWORKS_MODEL` | `accounts/fireworks/models/minimax-m3p1-flash` | modèle interrogé |
| var `FIREWORKS_BASE_URL` | `https://api.fireworks.ai/inference/v1/chat/completions` | point d'API |
| var `FIREWORKS_AGENT_RESOLVE` | `1` | `0` coupe l'agent sans toucher au code |

## Garde-fous

Empreinte par tête de PR + cooldown 20 min, plafond 10 saisines/heure (`SPAWN_CAP`)
et 3 saisines par PR en 30 min (`SAISINES_PR_MAX` : une PR ne peut pas brûler
tout le quota horaire à elle seule), 100 fichiers max, refus des conflits sous
`.github/`, des binaires et des fichiers de plus de 200 Ko, budget 240 s, push
avec bail uniquement. Le dry-run ne consomme ni empreinte ni quota, et l'union
déterministe des journaux ne consomme rien du tout.
Labels `hold` / `intervention-humaine` : main humaine, aucune saisine.
Succès : label `agent-conflit`. Le modèle ne voit que les bords du conflit,
jamais aucun secret.

Recette : `python3 conflict_agent.py resolve --dry-run --repo BaptisteFaisy/relais
--pr N` affiche les prompts sans rien appeler (dans un clone en conflit).

## Agent de périmètre (`scope_agent.py`)

Une PR dont le corps n'a pas de `## Scope` (ou dont la section ne couvre pas tous
les fichiers modifiés) est refusée par `couche0`, donc vue `BLOCKED` par le
balayeur. L'agent lit le diff, rédige une puce ``- `chemin` — raison`` par
fichier en français, puis **rejoue `check_scope` du garde de `main`** sur le
corps recomposé avant d'écrire : fichier non déclaré et déclaration inutile
restent refusés. Ce n'est pas un contournement du garde, c'est la déclaration
manquante qui est écrite.

```bash
FIREWORKS_API_KEY=... python3 scope_agent.py fix \
  --repo BaptisteFaisy/relais --pr 215 --sha <tête> --guard /tmp/check_couche0.py
```

Sorties : `0` corps complété (un commentaire porte le marqueur `automerge-scope`
avec la tête, donc une seule tentative par commit), `1` échec avec compte rendu,
`2` rien à faire. Réglages : `FIREWORKS_MODEL` / `FIREWORKS_BASE_URL` comme
l'agent de conflit, `FIREWORKS_AGENT_SCOPE=0` pour couper. Plafond 25 fichiers ;
au-delà, label `intervention-humaine` et main humaine.

Une puce rendue sous forme de glob ou de dossier (`app/**/Settings*.kt`,
`docs/`) est rapprochée du fichier avec la **règle du garde** (`matches`), pas
par égalité de chaîne : le 29/10, un glob juste était compté comme absent et la
PR restait bloquée sur « le modèle n'a rien rendu ». Si des fichiers manquent
encore, une **relance ciblée** ne redemande qu'eux, avec leur diff ; la réponse
du modèle est journalisée au moindre échec.

## Indice de triage (`--indice`)

Depuis le 01/11, l'agent ne part plus du seul journal. Le workflow
`triage-rouge` relance la classe tombée seule et écrit son verdict dans
`/tmp/triage.txt` (classe, périmètre, suspects) ; `test_agent.py fix --indice
/tmp/triage.txt` l'ajoute au prompt, sous « Triage automatique (déjà fait, ne le
refais pas) ».

C'est ce qui manquait sur #919 : la classe tombée n'était pas dans le diff, le
coupable était le nouveau test de la PR (globaux de séance sales), et l'agent —
lancé sans indice — a rendu « budget de 150 s dépassé », deux fois.

Sans `--indice`, le comportement est celui d'avant (journal seul). L'option est
facultative et ne casse aucun appelant : le balayeur peut la passer aussi.

## Validation

```bash
cd .github/fireworks
python3 -m unittest test_conflict_agent test_scope_agent \
    test_union_journal test_quota -v
```
