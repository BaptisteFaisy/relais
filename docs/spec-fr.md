# Relève entre deux calculateurs sur QNX 8.0 — cahier des charges

**Nom de travail : « Relais »** (dans une course de relais, un seul coureur porte le bâton à la fois, et il le passe sans s'arrêter).
Version 0.1 — 11 novembre 2026 — projet candidat au QNX Global Hackathon, catégorie Automobile & Mobilité.
Date limite de soumission : mardi 15 décembre 2026 à 20 h, heure de Paris.

---

## 0. Lecture rapide

**Ce que c'est.** Deux machines QNX 8.0 exécutent la même boucle de commande d'un actionneur de direction simulé. Une seule commande à la fois. Quand elle tombe en panne, l'autre prend la relève dans un délai borné et mesuré, sans à-coup. La machine évincée ne peut plus rien imposer, même si elle se réveille en se croyant encore aux commandes.

**La promesse, en une phrase.** Après n'importe quelle panne simple (machine morte, machine figée, câble coupé), l'actionneur ne reste jamais plus de trois périodes sans ordre frais au niveau socle (15 ms à 200 Hz), jamais une seule au niveau complet, et il n'applique jamais l'ordre d'une machine destituée. C'est démontré par exploration, pas seulement affirmé.

**Quatre idées qui portent le projet.**

1. **L'actionneur est l'arbitre.** C'est lui qui délivre les numéros de mandat, et il ne retire un mandat que sur son propre constat : le titulaire ne lui envoie plus rien. Deux machines qui ne s'entendent plus ne peuvent donc pas commander toutes les deux. Le dilemme « machine morte ou câble coupé ? » se règle sans troisième calculateur.
2. **L'actionneur donne le tempo.** Il émet un état à chaque période (le *tick*), et chaque ordre ne vaut que pour le tick qu'il nomme. Aucune horloge n'est à synchroniser entre les machines, et un ordre calculé avant un gel est périmé par construction.
3. **Le testament.** Chaque ordre embarque les valeurs prévues pour les ticks suivants. Pendant la relève, l'actionneur joue le plan du titulaire disparu : la courbe physique ne montre pas de trou.
4. **La preuve par l'exploration, et la preuve de la preuve.** Le cœur du protocole est une machine d'états pure, sans horloge ni réseau. On l'explore exhaustivement sur PC, puis on tue, on fige et on coupe pour de vrai sous QEMU, à chaque instant du cycle. Des versions volontairement défectueuses du logiciel (les *mutants*) vérifient que le banc d'essai voit bien les fautes.

**Trois niveaux d'ambition.** Chaque exigence porte un niveau : **S** (socle, indispensable à une soumission), **C** (complet, la version visée), **X** (extrême, ce qui pousse l'idée au bout).

**À valider avant d'écrire la moindre ligne de produit** (section 10, semaine 1) : six essais de faisabilité. Le plus important vérifie que deux machines QNX sous QEMU échangent des trames sans droits d'administrateur sur le PC, et à quelle latence.

**Statut des affirmations.** La section 14 sépare ce qui a été vérifié (texte du règlement, options de QEMU essayées sur ce PC, dépôts voisins lus) de ce qui reste une hypothèse de conception.

---

## 1. Contexte

### 1.1 Sûr à l'arrêt, ou sûr en marche

Tant qu'une colonne de direction relie le volant aux roues, la panne du calculateur d'assistance est supportable : l'assistance s'arrête et le conducteur tourne plus fort. Il suffit que le calculateur se taise proprement. On dit qu'il est *sûr à l'arrêt*.

Deux évolutions cassent ce raisonnement :

- **La direction sans liaison mécanique** (*steer-by-wire*). Sans colonne, un calculateur qui se tait laisse les roues sans commande.
- **La conduite automatisée.** Quand personne ne tient le volant, personne ne peut reprendre la main dans la seconde.

Dans les deux cas, se taire n'est plus un état sûr. La fonction doit continuer, au moins le temps de s'arrêter correctement. On dit que le système doit être *sûr en marche*. La réponse courante est de doubler le calculateur, et toute la difficulté se déplace vers une question : **qui commande, et comment change-t-on de main sans erreur et sans délai ?**

### 1.2 Cadre réglementaire et normatif

| Texte | Ce qu'il faut en retenir |
|---|---|
| **Règlement ONU n° 79** (équipement de direction), révision 4, série 03 d'amendements | Admet la direction dont l'effort vient uniquement d'une source d'énergie (*full-power steering*, §2.5.2.5), cadre de la direction sans liaison mécanique. Détail ci-dessous. |
| **Règlement ONU n° 79, annexe 6** | Exigences sur les « systèmes complexes de commande électronique » : le constructeur documente son concept de sécurité, sa stratégie en cas de défaut, et l'analyse qui le justifie. |
| **ISO 26262:2018** (sûreté de fonctionnement automobile) | Fournit le vocabulaire des délais : intervalle de tolérance au défaut, de détection, de réaction, et durée admise du fonctionnement de secours. Relevé sur des sources secondaires, la norme étant payante. |

**Règlement n° 79, points utiles à la conception** (numéros de paragraphes lus sur la révision 4 du 7 novembre 2018 ; traductions libres, à aligner sur la version française officielle et sur le texte en vigueur) :

- **§2.3.2.1 — transmission de commande** : tous les composants par lesquels passent les signaux de commande de la direction. Les deux calculateurs de ce projet en font partie.
- **§5.3.1.3** : toute défaillance d'une transmission autre que purement mécanique est clairement signalée au conducteur.
- **§5.3.3.1** : le véhicule ne doit pas pouvoir rouler indéfiniment à plus de 10 km/h avec un défaut qui allume le signal d'avertissement.
- **§5.3.3.2** : en cas de défaillance dans la transmission de commande, il doit rester possible de diriger le véhicule **avec les performances prévues pour le système intact**. C'est l'exigence de continuité.
- **§5.3.3.3** : en cas de défaillance de la source d'énergie de la transmission de commande, au moins 24 manœuvres en « huit » (boucles de 40 m de diamètre, à 10 km/h) restent possibles au niveau de performance du système intact.
- **§5.3.3.4** : en cas de défaillance de la transmission d'énergie, aucun changement immédiat de l'angle de braquage. C'est l'exigence d'absence d'à-coup.
- **§5.4.2.1.1 et §5.4.2.1.2** : un signal rouge pour les défaillances de l'équipement principal, un signal jaune pour un défaut détecté électriquement qui n'allume pas le rouge.
- **Annexe 6, §3.4.3** : parmi les dispositions possibles en cas de défaut, (b) « basculement sur un système de secours distinct ». **§3.4.3.2** : dans ce cas, le constructeur explique les principes du mécanisme de basculement, sa logique, son niveau de redondance, ses contrôles intégrés, et définit les limites d'efficacité du secours.
- **Annexe 6, §3.4.4** : l'analyse s'appuie sur une étude des modes de défaillance ou un arbre de défaillances, et sur une validation, par exemple par essais avec matériel dans la boucle.
- **Annexe 6, §4.1.2** : la réaction du système est vérifiée sous l'effet d'une défaillance.

Ce projet est, à petite échelle, la réponse au §3.4.3.2 : un mécanisme de basculement dont le principe, la logique, les contrôles et les limites sont écrits, puis vérifiés par injection de pannes.

**Vocabulaire ISO 26262 repris dans ce document** (d'après des sources secondaires) :

| Terme ISO | Sens | Équivalent dans Relais |
|---|---|---|
| Intervalle de tolérance au défaut (FTTI) | Temps entre l'apparition d'un défaut et un possible événement dangereux, si rien n'est fait | Horizon du testament : au-delà, l'actionneur n'a plus de plan |
| Intervalle de détection (FDTI) | Temps entre le défaut et sa détection | p+1 ticks |
| Intervalle de réaction (FRTI) | Temps entre la détection et l'état maîtrisé | Du constat au premier ordre du nouveau titulaire |
| Durée admise du fonctionnement de secours (EOTTI) | Temps pendant lequel on peut rouler en secours sans risque déraisonnable | Durée limitée du mode dégradé (DEG-05) |

**Exemples de série** (d'après la presse spécialisée et Wikipédia, non vérifiés sur des documents de constructeur) : l'Infiniti Q50 de 2013, première voiture de série à direction sans liaison mécanique permanente, embarquait trois calculateurs et gardait un embrayage qui rétablissait une liaison mécanique en cas de défaut. Le Tesla Cybertruck n'a pas de secours mécanique et s'appuie sur des moteurs doublés et des capteurs triplés.

### 1.3 Quatre pièges

1. **Morte ou muette ?** Une machine qui n'entend plus sa voisine ne sait pas si elle est morte ou si le câble est coupé. Si elle prend la main dans le second cas, deux machines commandent. C'est la *double commande*.
2. **Le revenant.** Une machine figée quelques centaines de millisecondes (surcharge, défaut matériel passager, gel d'une machine virtuelle) reprend exactement où elle s'était arrêtée. Elle ignore que du temps a passé et envoie un ordre calculé pour une situation qui n'existe plus.
3. **La secousse.** Si la remplaçante repart d'un état différent (intégrateur, filtre, position dans la trajectoire), l'ordre saute au moment du changement.
4. **La relève à tort.** Un délai de silence trop court destitue une machine saine qui a seulement eu un retard. Un délai trop long laisse l'actionneur sans commande.

### 1.4 Pourquoi un système temps réel à micronoyau

- **Détecter une panne par le silence ne marche que si les retards sont bornés.** Plus la latence d'un système est incertaine, plus le délai de silence doit être long pour ne pas destituer une machine saine. Les deux projets voisins lus règlent ce délai à 100 ms et à 2 000 ms (section 2). Relais vise deux périodes, soit 10 ms. Cet écart n'est tenable que si le pire retard de réveil d'une tâche est petit et connu.
- **Chaque rôle est un processus isolé.** Réseau, protocole et loi de commande sont séparés. Le plantage de l'un est connu des autres par la fermeture de sa connexion, et la machine cède la main dans la période au lieu d'attendre un délai de silence.
- **Le chemin critique reste prioritaire de bout en bout.** De la trame reçue à l'ordre émis, les échanges entre processus se font par passage de messages avec héritage de priorité. Cela se montre sur une trace noyau.
- **La pile réseau est elle aussi un processus** (`io-sock`). La priorité à laquelle elle traite les trames du protocole est une hypothèse à mesurer (essai E5).

### 1.5 Cadre du hackathon

- **Catégorie visée** : Automobile & Mobilité. Sur les 133 dépôts publics visibles au 11 novembre 2026, aucun ne décrit une relève entre deux machines.
- **Critères du jury**, à poids égal : réalisation technique, produit complet et cohérent, impact potentiel (problème réel dans un marché QNX, et pourquoi le temps réel), innovation et créativité, y compris dans l'usage de l'IA.
- **À fournir** : dépôt public neuf sous licence libre, description, instructions de construction précisant la version de QNX, la cible et la chaîne de compilation, vidéo de moins de trois minutes sur YouTube, le tout en anglais.
- **Prix accessibles** : catégorie, grand prix, et deux bonus cumulables : meilleur usage des technologies propres à QNX, meilleur tutoriel.

| Critère du jury | Réponse du projet |
|---|---|
| Réalisation technique | Arbitrage par l'actionneur, ordres adressés à un tick, réplication d'état sans à-coup, cœur pur exploré exhaustivement, campagne de pannes réelles |
| Produit complet | Du lancement en une commande au rapport de campagne, avec outil de service, tableau de bord et tutoriel |
| Impact | Direction sans liaison mécanique et conduite automatisée : la fonction doit survivre à la panne d'un calculateur, et le règlement demande d'expliquer le mécanisme de basculement |
| Innovation | L'actionneur comme arbitre et comme horloge ; le testament ; les mutants qui prouvent le banc ; au niveau extrême, la recherche automatique du pire scénario de panne |

---

## 2. Positionnement par rapport à l'existant

Dépôts lus le 11 novembre 2026. Aucun ne fait de relève entre deux machines.

| Projet (GitHub, public) | Ce qu'il fait | Ce qu'il ne fait pas |
|---|---|---|
| `TheUndefinedEngineer/Medical_Infusion_Pump` (avril 2026) | Pompe à perfusion sur une carte Raspberry Pi 5 : six processus, dont un capteur principal et un capteur de secours. Battement toutes les 200 ms, délai de silence de 2 000 ms, promotion du secours annoncée en moins d'une milliseconde. Démonstrations par arrêt forcé de processus. | Redondance entre processus d'une même machine. Aucune protection décrite contre deux actifs à la fois. Pas de banc automatisé. |
| `AhmedAdelWafdy7/autoware-qnx-guardian` (hackathon) | Un invité QNX surveille un invité Linux qui fait tourner la conduite automatisée. Délai de silence de 100 ms. En cas de panne, il arrête le véhicule par une manœuvre à risque minimal. | Il arrête, il ne continue pas : c'est du *sûr à l'arrêt*. Pas de mesure de délai ni d'injection de pannes à ce jour, d'après son README. |
| `zuhairhalawa/faultline` (hackathon) | Banc d'injection de fautes sur une cible : plantage, gel, vol de processeur, capteur lent ou bloqué. Un superviseur relance les processus. Verdict par étape, rapport. | Une seule machine. Aucune relève, aucun second. |
| `Lainholic/qnx-safepulse-lab` (hackathon) | Un surveillant relance un pilote planté (61 à 89 ms mesurés sur 50 plantages) et passe en arrêt d'urgence après 50 ms de silence du bus. | Une seule machine. |
| `atss0/qnx-hotswap` (hackathon) | Met à jour un pilote en marche sans que ses clients le voient (d'après sa description, dépôt non lu en détail). | Continuité pendant une mise à jour voulue, pas pendant une panne. |

**Hors QNX**, cités de mémoire comme points de repère et non revérifiés : les gestionnaires de grappes de serveurs, qui isolent la machine douteuse en lui coupant le courant ; les adresses IP flottantes, dont la bascule se compte en secondes ; la réplication de machines virtuelles de QEMU (COLO) ; les bus automobiles et aéronautiques à déclenchement temporel ; les paires commande-surveillance des calculateurs de vol.

**Ce qui doit distinguer Relais, sans ambiguïté :**

1. Deux machines, pas deux processus : la machine entière peut mourir, se figer ou perdre un câble.
2. Continuer, pas s'arrêter : la commande ne s'interrompt pas.
3. La double commande rendue impossible par construction, puis vérifiée par exploration exhaustive.
4. Une borne mesurée sur tous les instants de panne du cycle, pas sur quelques essais.
5. Un banc qui se teste lui-même par des mutants.

**Règle de prudence.** Le projet se présente comme un démonstrateur « aligné sur » l'annexe 6 du règlement n° 79. Il ne se dit jamais conforme ni homologable. Les délais sont des pires cas mesurés sous émulation, jamais des pires cas prouvés.

---

## 3. Périmètre

### 3.1 Objectifs

- Commander un actionneur simulé depuis deux machines QNX, une seule à la fois.
- Tenir une borne mesurée sur l'intervalle entre deux ordres frais, quelle que soit la panne simple et quel que soit son instant.
- Rendre la double commande impossible, y compris face à une machine qui se réveille après un gel.
- Démontrer tout cela par des essais automatisés, reproductibles par le jury sur un PC, sans matériel et sans droits d'administrateur.

### 3.2 Hors périmètre

- Actionneur réel, électronique de puissance, alimentation doublée.
- Tolérance à une machine qui calcule faux : le projet détecte le désaccord (niveau complet), il ne garantit pas de le trancher à deux machines.
- Défaut commun aux deux exemplaires du même logiciel. Une doublure dissemblable est une extension (section 12).
- Attaquant sur le réseau au-delà de l'authentification des trames.
- Redondance des capteurs : la position et la consigne viennent de l'actionneur simulé.
- Certification de sûreté de fonctionnement et homologation.

### 3.3 Modèle de pannes

| Code | Panne | Traitement |
|---|---|---|
| F1 | Mort d'une machine (arrêt net) | Tolérée |
| F2 | Gel puis reprise, durée quelconque | Tolérée |
| F3 | Machine lente, qui manque des ticks | Tolérée ; destituée si les manques se suivent |
| F4 | Coupure du lien entre calculateurs, dans un sens ou dans les deux | Tolérée sans relève ; redondance signalée comme dégradée |
| F5 | Coupure du lien entre une machine et l'actionneur, dans un sens ou dans les deux | Tolérée ; relève si c'était le titulaire |
| F6 | Perte, duplication, désordre, retard de trames | Tolérés dans les bornes des paramètres |
| F7 | Trame corrompue ou forgée | Détectée, traitée comme une perte |
| F8 | Valeur fausse émise par une machine | Détectée au niveau complet, non tolérée |
| F9 | Seconde panne avant le retour de la redondance | État sûr de l'actionneur |
| F10 | Mort des deux machines | État sûr de l'actionneur |
| F11 | Panne de l'actionneur lui-même | Hors périmètre (extension : deux voies) |

**Hypothèse de panne simple.** Une panne à la fois, jusqu'au retour de la redondance. Les pannes doubles ne sont pas tolérées, mais elles doivent mener à l'état sûr, et c'est testé.

### 3.4 Hypothèses techniques

- QNX SDP 8.0 obtenu par QNX Everywhere, image construite avec `mkqnximage`, cible QEMU x86_64. Les deux machines démarrent de la même image et se distinguent par l'adresse MAC de leur carte réseau.
- QEMU 8.2 ou plus récent sur un PC Linux, avec accélération matérielle (KVM) pour les mesures de temps. Aucun droit d'administrateur requis.
- Hypothèses de temps, à mesurer et non à supposer : un délai de trame Δ d'au plus 1 ms sur un lien sain, un temps de calcul C d'au plus 0,5 ms, une gigue J d'au plus 1 ms.
- L'actionneur simulé est réputé fiable. Sa logique d'arbitrage est volontairement minuscule pour pouvoir être explorée en entier.

---

## 4. Conception proposée

Cette section décrit une solution. Elle n'a pas valeur d'exigence : seules les sections 5 à 7 engagent.

### 4.1 Les acteurs et le banc

```
                PC Linux, sans droits d'administrateur

   ┌──────────────┐     lien entre calculateurs     ┌──────────────┐
   │   QEMU  A    │                                 │   QEMU  B    │
   │   QNX 8.0    │◄───────►┌───────────┐◄─────────►│   QNX 8.0    │
   │   titulaire  │         │   câble   │           │   second     │
   └──────┬───────┘         │ programmé │           └───────┬──────┘
          │ lien A          └───────────┘                   │ lien B
          ▼                                                 ▼
   ┌───────────────────────────────────────────────────────────────┐
   │  actionneur simulé : arbitre + modèle de crémaillère + tempo  │
   └───────────────────────────┬───────────────────────────────────┘
                               │ journal d'un enregistrement par tick
   ┌──────────────┐            ▼
   │ orchestrateur│──► QEMU : mort, gel, dégel, câble débranché
   └──────────────┘──► vérificateur d'invariants ──► rapport
```

| Acteur | Rôle |
|---|---|
| Machines A et B | Deux QNX identiques. Chacune calcule la loi de commande. L'une est titulaire du mandat, l'autre est second. |
| Actionneur | Simule la crémaillère, donne le tick, arbitre le mandat, applique une valeur par tick, tient le journal qui fait foi. Sur le PC au socle, dans une troisième machine QNX au niveau complet. |
| Câble programmé | Petit relais sur le PC entre les deux QEMU. Il laisse passer, perd, retarde, duplique ou désordonne les trames, par type de message et par sens. |
| Orchestrateur | Lance les machines, déroule un plan de pannes, pilote QEMU (mort, gel, dégel, câble débranché), collecte journaux et captures. |
| Vérificateur | Relit le journal de l'actionneur et contrôle les invariants. |

### 4.2 Le tick : l'actionneur donne le tempo

```
   tick n                              tick n+1
     │ l'actionneur applique la valeur   │ l'actionneur applique la valeur
     │ destinée au tick n                │ destinée au tick n+1
     │ STATUT(n) ───────────► A et B     │ STATUT(n+1) ───────────► A et B
     │       A calcule
     │       A ── ORDRE(n+1) ──► actionneur
     │       A ── ETAT(n+1) ───► B
     │       B ── OMBRE(n+1) ──► actionneur   (niveau complet)
```

- Les machines ne se réveillent pas sur leur propre horloge mais sur l'arrivée du `STATUT`. Aucune synchronisation d'horloges n'est nécessaire.
- Un ordre nomme le tick auquel il est destiné. L'actionneur écarte tout ordre qui vise un autre tick que le prochain.
- **Ordre frais** : ordre calculé à partir du `STATUT` du tick précédent. C'est l'unité de mesure du projet.
- Contrainte de cycle : l'aller, le calcul et le retour doivent tenir dans la période, soit 2Δ + C + J ≤ T. Avec les hypothèses de la section 3.4 : 3,5 ms pour une période de 5 ms.

### 4.3 Le mandat

L'actionneur tient deux variables que lui seul modifie : le **numéro de mandat** et le **titulaire**.

| Règle | Énoncé |
|---|---|
| Acceptation | Un `ORDRE` n'est appliqué que s'il vient du titulaire, porte le mandat courant et vise le tick à venir. |
| Retrait | Le mandat n'est retiré que sur le constat de l'actionneur : p+1 ticks de suite sans ordre valide du titulaire (p = 1 par défaut), ou une `CESSION` reçue de lui. |
| Attribution, socle | Une machine non titulaire envoie une `DEMANDE`. L'actionneur l'accorde si le mandat est vacant, la refuse sinon. La demande peut embarquer un premier ordre. |
| Attribution, complet | Le second envoie un `OMBRE` à chaque tick. L'actionneur le *certifie prêt* quand ses ombres concordent avec les ordres du titulaire. Au retrait du mandat, il bascule de lui-même vers le second certifié. |
| Numérotation | Le numéro augmente de 1 à chaque changement de titulaire et ne décroît jamais. Une machine ne peut pas s'en attribuer un. |
| Publication | Chaque `STATUT` annonce le mandat, le titulaire et la source de la valeur appliquée. Une machine destituée l'apprend là. |

**Pourquoi la double commande est impossible.** Deux machines peuvent se *croire* titulaires pendant un instant. Cela n'a aucun effet : l'actionneur n'applique que l'ordre du titulaire qu'il a lui-même désigné, pour le tick à venir. La sûreté ne dépend ni de l'horloge des machines ni du délai des trames. Le temps ne joue que sur la rapidité de la relève.

### 4.4 Ce que l'actionneur applique à chaque tick

Une seule valeur, choisie dans cet ordre :

1. l'`ORDRE` du titulaire pour ce tick ;
2. à défaut, l'`OMBRE` du second certifié pour ce tick (la *suppléance*, niveau complet) ;
3. à défaut, la valeur prévue par le testament du dernier ordre reçu, si son horizon n'est pas dépassé ;
4. à défaut, la *loi de secours* : une boucle locale amortie qui maintient la position, témoin rouge allumé.

### 4.5 Mort du titulaire, tick par tick

Cas le plus défavorable : A meurt juste après avoir émis son ordre pour le tick n. Paramètres par défaut (p = 1).

| Tick | Reçu par l'actionneur | Socle : valeur appliquée | Complet : valeur appliquée |
|---|---|---|---|
| n | `ORDRE` de A | Ordre de A (frais) | Ordre de A (frais) |
| n+1 | Rien de A | Testament de A, premier manque | `OMBRE` de B (frais), première suppléance |
| n+2 | Rien de A ; `DEMANDE` de B au socle | Testament de A, mandat déclaré vacant puis accordé à B | `OMBRE` de B (frais) ; le mandat passe à B |
| n+3 | `ORDRE` de B | Ordre de B (frais), mandat suivant | Ordre de B (frais) |

- **Socle** : au plus 3 périodes entre deux ordres frais, soit 15 ms, comblées par le testament. Une variante où le second demande dès le premier manque ramène ce délai à 2 périodes.
- **Complet** : aucun tick sans ordre frais. Le changement de mandat devient une formalité, effectuée 2 périodes après le dernier ordre de A.
- **Double panne** : si B meurt aussi, la loi de secours s'applique au plus N+1 ticks après le dernier ordre frais (N = 8, soit 45 ms).

### 4.6 Le revenant

```
  temps ────────────────────────────────────────────────────────────────►
  A   titulaire, mandat 7 │ figée ░░░░░░░░░░░░░░░░░ │ dégelée : envoie ORDRE(mandat 7, tick ancien)
  B   second ─────────────┴ supplée, puis titulaire du mandat 8 ────────────────────────────────
  ACT applique A          │ applique B ; mandat 7 → 8            │ écarte A ; A lit le STATUT, se range second
```

Au dégel, A ne sait pas forcément que du temps a passé. Son ordre est écarté deux fois : il ne vient plus du titulaire, et il vise un tick révolu. A découvre dans le `STATUT` suivant que B tient le mandat 8. Elle cesse d'émettre des ordres et se resynchronise comme second.

Un gel plus court que p+1 ticks ne change rien : la suppléance ou le testament comblent, et A reste titulaire.

### 4.7 Table de vérité des pannes

| Panne | Ce que voit l'actionneur | Ce que voit l'autre machine | Issue attendue |
|---|---|---|---|
| A (titulaire) meurt | Plus d'ordre de A | Plus d'`ETAT` | Relève vers B ; témoin jaune |
| A se fige, puis revient | Plus d'ordre, puis des ordres périmés | Plus d'`ETAT` | Relève vers B ; ordres de A écartés ; A se range second |
| A se fige moins de p+1 ticks | Un manque isolé | Un `ETAT` manquant | Pas de relève ; rien à signaler |
| B (second) meurt | Plus d'`OMBRE` | Plus d'acquit de réplique | Pas de relève ; témoin jaune « second absent » |
| Lien entre calculateurs coupé | Rien d'anormal | Chacune n'entend plus l'autre | Pas de relève ; B suit par les `STATUT` ; témoin jaune « lien perdu » |
| Lien A–actionneur coupé, deux sens | Plus d'ordre de A | `STATUT` : manques du titulaire | Relève vers B ; A se déclare « sans actionneur » |
| A n'est plus entendue de l'actionneur | Plus d'ordre de A | Idem | Relève vers B ; A l'apprend par le `STATUT` |
| A n'entend plus l'actionneur | Plus d'ordre de A (elle n'a plus de tempo) | `ETAT` figé | Relève vers B ; A se destitue d'elle-même |
| Lien B–actionneur coupé | Plus d'`OMBRE` | `STATUT` : second non certifié | Pas de relève ; témoin jaune |
| A lente, manques isolés | Manques non consécutifs | — | Pas de relève ; compteur publié |
| A émet des valeurs fausses | `ORDRE` différent de l'`OMBRE` | — | Alerte « désaccord », témoin rouge |
| A et B meurent | Plus rien | — | Testament, puis loi de secours ; témoin rouge |
| Trame corrompue ou forgée | Contrôle invalide | — | Écartée et comptée |

### 4.8 Réplication d'état et absence d'à-coup

- L'état de la loi de commande est petit (intégrateur, filtres, position dans la trajectoire : quelques dizaines d'octets). Le titulaire l'envoie **en entier à chaque tick** dans un `ETAT`. Il n'y a ni différentiel, ni session, ni rattrapage.
- Le second remplace son état par celui reçu, puis calcule sur les mêmes mesures que le titulaire. Même binaire, mêmes entrées : la même valeur est attendue au bit près, ce qui permet à l'actionneur de comparer ordre et ombre.
- Si le lien entre calculateurs est coupé, le second reconstruit un état sans à-coup à partir de la valeur appliquée et de la position que l'actionneur publie.
- La mesure de continuité est un **jumeau sans panne** : le même scénario, rejoué sans panne par le simulateur, donne tick par tick la valeur et la position de référence. L'écart à ce jumeau quantifie l'effet physique de chaque panne.

### 4.9 Architecture logicielle d'une machine

```
          trames UDP                 messages QNX                 messages QNX
  liens ◄──────────► ┌────────────┐ ───────────► ┌─────────────┐ ───────────► ┌────────────┐
                     │ relais-net │              │ relais-core │              │ relais-ctl │
                     │ (prio 62)  │ ◄─────────── │ (prio 61)   │ ◄─────────── │ (prio 60)  │
                     └────────────┘              └──────┬──────┘              └────────────┘
                                                        │ /dev/relais/status, temoin, ctl
                                                 ┌──────┴──────┐
                                                 │  relaisctl  │
                                                 └─────────────┘
```

| Processus | Rôle | Priorité | Peut accéder à |
|---|---|---|---|
| `relais-net` | Seul propriétaire des sockets. Contrôle la forme et l'authentifiant des trames, les remet au cœur. | 62 | Le réseau, le canal du cœur |
| `relais-core` | Gestionnaire de ressources. Exécute la machine d'états du protocole, tient le rôle et la réplique d'état. | 61 | Les canaux de `relais-net` et de `relais-ctl` |
| `relais-ctl` | Loi de commande : fonction pure de l'état et des mesures. Ne connaît ni le réseau ni le rôle. | 60 | Le canal du cœur |
| `relaisctl` | Outil de service : état, témoin, relève à la demande, commandes de test. | 10 | `/dev/relais` |

**Échelle de réaction.** Un processus qui plante est relancé sur place, et la machine cède le mandat dans le tick. Une machine entière qui disparaît est remplacée par la relève. Les deux mécanismes se complètent : le premier est plus rapide, le second couvre tout.

### 4.10 Un cœur pur, trois usages

La logique du protocole (arbitre de l'actionneur, machine d'états d'un calculateur, encodage des trames) est une bibliothèque C portable. Elle ne lit aucune horloge, n'ouvre aucun fichier ni socket, n'alloue rien : elle reçoit un événement et rend un nouvel état et des actions.

Les mêmes fichiers sont compilés trois fois : dans les processus QNX, dans le simulateur sur PC, et dans l'explorateur exhaustif. Ce qui est exploré est donc le code qui tourne, pas un modèle écrit à côté.

### 4.11 Moyens d'injection sous QEMU

| Panne | Moyen | Statut sur ce PC |
|---|---|---|
| Mort d'une machine | Arrêt brutal du processus QEMU, ou commande `quit` | Commande présente |
| Mort à un point précis du code | Écriture sur le périphérique `isa-debug-exit` depuis l'invité | Périphérique accepté ; accès depuis QNX à essayer (E4) |
| Gel et dégel | Commandes `stop` et `cont` | Commandes présentes |
| Câble débranché | Commande `set_link` | Exécutée avec succès sur une machine à l'arrêt |
| Silence sans perte de porteuse, perte, retard, désordre | Câble programmé entre deux liens `-netdev dgram` | Syntaxe acceptée ; échange de trames à essayer (E1) |
| Redémarrage à chaud | Commande `system_reset` | Commande présente |
| Preuve | Capture de chaque lien par `filter-dump` | Fichier de capture créé |

---

## 5. Exigences

Niveau : **S** socle, **C** complet, **X** extrême. Vérification : **T** test automatique, **M** mesure, **I** inspection du code, **D** démonstration.

### 5.1 Boucle de commande et modèle

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| CMD-01 | L'actionneur cadence le système : il émet un `STATUT` à chaque tick de période T (5 ms par défaut, réglable de 1 à 50 ms). | S | T |
| CMD-02 | À la réception du `STATUT` du tick n, chaque machine calcule la valeur destinée au tick n+1 et l'émet avant la fin de la période. | S | T |
| CMD-03 | La loi de commande est une fonction pure de l'état et des mesures : mêmes entrées, mêmes sorties au bit près sur les deux machines. | S | T, I |
| CMD-04 | Le chemin du `STATUT` reçu à l'ordre émis ne fait ni allocation mémoire, ni défaut de page, ni accès disque, ni attente sur un processus moins prioritaire. | S | I, M |
| CMD-05 | Le modèle de l'actionneur (crémaillère du second ordre avec perturbation) et les trajectoires de référence (slalom, changement de voie, échelon) sont fournis et paramétrables par fichier. | S | D |
| CMD-06 | L'ordre est borné en amplitude et en vitesse de variation par la machine émettrice. | S | T |
| CMD-07 | Un mode « ralenti » multiplie T (par 10 par défaut) sans changer la logique : tous les délais du protocole s'expriment en ticks. | C | T |
| CMD-08 | Un second profil de démonstration commande un freinage, avec sa propre loi de secours. | C | D |
| CMD-09 | La loi de commande s'échange sans toucher au protocole, par une interface à trois fonctions : initialiser, calculer, exporter l'état. | X | T |

### 5.2 Mandat et arbitrage

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| MAN-01 | L'actionneur tient un numéro de mandat sur 32 bits et l'identité du titulaire. Lui seul les modifie. | S | T, I |
| MAN-02 | L'actionneur n'applique un `ORDRE` que s'il vient du titulaire, porte le mandat courant et vise le tick à venir. Tout autre ordre est écarté et compté. | S | T |
| MAN-03 | Le numéro de mandat ne décroît jamais et augmente d'exactement 1 à chaque changement de titulaire. | S | T |
| MAN-04 | L'actionneur ne retire le mandat que sur son propre constat : p+1 ticks consécutifs sans ordre valide du titulaire (p = 1 par défaut), ou une `CESSION` reçue de lui. | S | T |
| MAN-05 | Une machine non titulaire obtient le mandat par une `DEMANDE`. L'actionneur l'accorde si le mandat est vacant et la refuse sinon. La demande peut embarquer le premier ordre. | S | T |
| MAN-06 | Au démarrage, le mandat est vacant. La première demande valide l'obtient ; l'autre machine devient second. | S | T |
| MAN-07 | Chaque `STATUT` publie le mandat, le titulaire, la source de la valeur appliquée et le nombre de manques consécutifs. | S | T |
| MAN-08 | Une machine qui se voit destituée dans un `STATUT`, ou refusée, cesse d'émettre des `ORDRE` avant le tick suivant. | S | T |
| MAN-09 | Une machine privée de `STATUT` pendant 4 ticks se considère destituée. La sûreté ne dépend pas de cette règle (MAN-02 suffit) ; elle sert au diagnostic. | C | T |
| MAN-10 | Le second certifié émet un `OMBRE` à chaque tick. L'actionneur bascule de lui-même le mandat vers lui après p+1 manques du titulaire, sans échange supplémentaire. | C | T |
| MAN-11 | Le numéro de mandat survit au redémarrage de l'actionneur. | C | T |
| MAN-12 | Tout effet externe d'une machine autre que l'ordre (témoin, journal partagé, message à un tiers) porte le numéro de mandat, pour que son destinataire écarte celui d'une machine destituée. | C | T, I |
| MAN-13 | Avec deux voies d'actionneur, les deux mandats vont à la même machine, ou la divergence est signalée en un tick. | X | T |

### 5.3 Détection

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| DET-01 | L'actionneur constate l'absence d'ordre tick par tick. Aucun délai de détection ne dépend de l'horloge d'une machine. | S | T, I |
| DET-02 | Le second soupçonne le titulaire quand il ne reçoit pas d'`ETAT` pendant un tick, ou quand un `STATUT` annonce un manque. Un soupçon n'a aucun effet sans l'accord de l'actionneur. | S | T |
| DET-03 | La perte de p ordres consécutifs ne provoque pas de changement de titulaire. | S | T |
| DET-04 | Une trame au contrôle invalide, de taille ou de version inattendue, est traitée comme perdue et comptée. | S | T |
| DET-05 | Une machine qui constate la mort d'un de ses processus critiques émet une `CESSION` dans le tick, sans attendre un délai de silence. | C | T |
| DET-06 | Coupure du seul lien entre calculateurs : aucune relève ; l'état « lien perdu » est signalé en 2 ticks. | C | T |
| DET-07 | Coupure du seul lien entre le titulaire et l'actionneur : relève vers le second ; l'ancien titulaire se déclare « sans actionneur ». | C | T |
| DET-08 | Coupure dans un seul sens : l'issue est celle de la table 4.7, vérifiée pour les six demi-liens. | C | T |
| DET-09 | L'actionneur compare à chaque tick l'`ORDRE` du titulaire et l'`OMBRE` du second. Un écart supérieur à ε pendant 3 ticks lève l'alerte « désaccord ». | C | T, M |
| DET-10 | Sur désaccord, un contrôle de vraisemblance par un modèle réduit désigne la machine fautive quand c'est possible. Sinon l'actionneur garde le titulaire et allume le témoin rouge. | X | T |

### 5.4 Relève

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| REL-01 | Après la mort du titulaire, le premier ordre frais du nouveau titulaire est appliqué au plus p+2 ticks après le dernier ordre frais de l'ancien (3 ticks, soit 15 ms à 200 Hz). | S | T, M |
| REL-02 | Entre-temps, l'actionneur applique le testament du titulaire disparu. | S | T |
| REL-03 | Le premier ordre du nouveau titulaire part de l'état répliqué : son écart avec la valeur du testament au même tick ne dépasse pas δ (2 % de la pleine échelle par défaut). | S | T |
| REL-04 | Une machine ne peut pas déclencher seule une relève : il faut le constat de l'actionneur. | S | T |
| REL-05 | Avec un second certifié, aucun tick n'est servi sans ordre frais : l'`OMBRE` supplée dès le premier manque, et le mandat bascule au tick p+1. | C | T, M |
| REL-06 | Relève consentie : sur `CESSION`, le mandat passe au second certifié au tick suivant. | C | T |
| REL-07 | Un ancien titulaire qui réapparaît voit ses ordres écartés et se range comme second, sans intervention. | C | T |
| REL-08 | Deux relèves successives (de A vers B, puis de B vers A après réintégration) se font sans redémarrer l'actionneur et sans tick perdu. | C | T |
| REL-09 | `relaisctl handover` déclenche une relève consentie, pour la maintenance. | C | D |
| REL-10 | Mise à jour sans interruption : on met à jour le second, on lui cède le mandat, on met à jour l'autre. Aucun tick sans ordre frais pendant l'opération. | X | T |

### 5.5 Réplication d'état et continuité

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| REP-01 | Le titulaire envoie au second, à chaque tick, un `ETAT` complet de 256 octets au plus. | S | T |
| REP-02 | Le second remplace son état par celui reçu, puis calcule. Un `ETAT` plus ancien que le dernier reçu est écarté. | S | T |
| REP-03 | L'âge de la réplique, en ticks, figure dans chaque `OMBRE` et chaque `DEMANDE`. | S | T |
| REP-04 | Sans `ETAT`, le second reconstruit un état sans à-coup à partir de la valeur appliquée et de la position publiées par l'actionneur. | C | T |
| REP-05 | L'actionneur certifie un second « prêt » après 3 ticks consécutifs où son `OMBRE` égale l'`ORDRE` à ε près. Il retire la certification au premier écart, ou après plus de p ombres manquantes. | C | T |
| REP-06 | Sur toute la campagne, l'écart entre la valeur appliquée et celle du jumeau sans panne reste sous δ, et l'écart de position sous une borne fixée après les essais de faisabilité. | C | T, M |
| REP-07 | Quand deux liens entre calculateurs existent, l'`ETAT` part sur les deux ; le second garde le premier exemplaire valide. | C | T |
| REP-08 | À trois machines, l'actionneur applique la valeur sur laquelle deux machines s'accordent. | X | T |

### 5.6 Actionneur et état sûr

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| ACT-01 | À chaque tick, l'actionneur applique exactement une valeur, choisie dans l'ordre de la section 4.4, et journalise sa source. | S | T |
| ACT-02 | Chaque `ORDRE` embarque un testament de N valeurs (N = 8). L'actionneur ne joue jamais une valeur au-delà de l'horizon du testament le plus récent. | S | T |
| ACT-03 | À l'épuisement du testament, la loi de secours s'applique au tick suivant et le témoin rouge s'allume. | S | T |
| ACT-04 | La sortie de la loi de secours se fait à vitesse de variation limitée, sur un ordre valide d'un titulaire. | S | T |
| ACT-05 | L'actionneur borne toute valeur en amplitude et en vitesse de variation, quelle que soit sa source. | S | T |
| ACT-06 | La logique d'arbitrage est une bibliothèque C portable de moins de 500 lignes, sans allocation, sans horloge ni entrée-sortie, identique sur PC et sur QNX. | S | I |
| ACT-07 | L'actionneur écrit un journal d'un enregistrement par tick et d'un enregistrement par événement (section 6.4). | S | T |
| ACT-08 | L'actionneur tourne aussi dans une troisième machine QNX, pour que le tempo soit donné par un système temps réel. | C | D |
| ACT-09 | Un ordre retardé par le réseau au-delà de son tick est écarté même s'il vient du titulaire. | C | T |
| ACT-10 | Deux voies d'actionneur, chacune avec son arbitre : la perte d'une voie est tolérée. | X | T |

### 5.7 Mode dégradé, signalisation et réintégration

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| DEG-01 | Trois états de témoin, publiés dans le `STATUT` et sous `/dev/relais/temoin` : éteint (redondance complète), jaune (redondance perdue, commande assurée), rouge (loi de secours ou désaccord). | S | T |
| DEG-02 | Le témoin reste allumé tant que la cause dure, et la cause est lisible : machine absente, lien perdu, second non certifié, désaccord, loi de secours. | S | T |
| DEG-03 | Une machine redémarrée rejoint comme second sans perturber la commande : pendant sa réintégration, chaque tick est servi par le titulaire. | S | T |
| DEG-04 | Une machine redevient second certifié moins de 2 s après la fin de son démarrage (cible). | C | M |
| DEG-05 | Durée limitée du mode dégradé : après 30 s sans redondance (valeur de démonstration), le système lève « arrêt demandé », à l'image de la règle qui interdit de rouler indéfiniment avec un défaut signalé. | C | T |
| DEG-06 | Seconde panne en mode dégradé : testament, puis loi de secours en N+1 ticks au plus. | C | T |
| DEG-07 | Une machine destituée ne redevient éligible qu'après une nouvelle certification : le mandat ne fait pas d'allers-retours à chaque tick. | C | T |
| DEG-08 | Une doublure dissemblable (loi de commande simplifiée, écrite séparément) peut tenir le rôle de second, pour couvrir un défaut commun aux deux exemplaires du logiciel. | X | T |

### 5.8 Temps réel

Les valeurs sont des cibles initiales, à ajuster après les essais de faisabilité. Elles s'entendent comme **pire cas mesuré**, pas comme pire cas prouvé.

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| PERF-01 | Du `STATUT` reçu par la machine à l'ordre émis : 1 ms au 99,9e centile, 2 ms au pire, sous charge processeur de priorité inférieure. | S | M |
| PERF-02 | Hors panne injectée, moins d'un tick sur 10 000 est servi autrement que par l'ordre du titulaire, sur 30 minutes. | S | M |
| PERF-03 | Sur 200 morts du titulaire, l'intervalle entre deux ordres frais ne dépasse jamais p+2 ticks. | S | M |
| PERF-04 | Avec un second certifié : zéro tick sans ordre frais sur 2 000 morts et 10 000 gels. | C | M |
| PERF-05 | Le changement de mandat intervient exactement p+1 ticks après le dernier ordre du titulaire. | C | M |
| PERF-06 | Les mêmes mesures tiennent sous trois charges : processeur saturé par des tâches moins prioritaires, rafale de trames sur les liens, pression mémoire. | C | M |
| PERF-07 | Les trois pires cas de PERF-01 sont expliqués par une trace noyau qui montre où le temps est passé. | C | D |
| PERF-08 | La période est abaissée jusqu'au point de rupture. La plus petite période pour laquelle PERF-02 tient est publiée. | C | M |
| PERF-09 | Les mêmes mesures sont refaites sur deux cartes réelles reliées par Ethernet. | X | M |

Sous QEMU, les deux machines partagent le processeur du PC : les durées ne représentent pas un matériel réel. Elles sont présentées comme émulées, avec leur distribution complète.

### 5.9 Robustesse

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| ROB-01 | Les invariants de la section 7.2 tiennent pour toute panne simple du modèle, à tout instant. | S | T |
| ROB-02 | Des trames dupliquées, désordonnées ou retardées de moins d'un tick n'ont aucun effet sur la commande. | S | T |
| ROB-03 | La perte de 10 % des trames d'un lien, sans rafale de plus de p, ne change pas le titulaire. | S | T |
| ROB-04 | Une panne pendant une relève, une réintégration ou une cession respecte les invariants (deux pannes imbriquées au plus). | C | T |
| ROB-05 | Un gel de durée quelconque, d'un tick à dix minutes, de l'une ou l'autre machine dans l'un ou l'autre rôle, respecte les invariants. | C | T |
| ROB-06 | Le redémarrage à chaud du titulaire est traité comme sa mort, puis comme une réintégration. | C | T |
| ROB-07 | Le plantage de chaque processus de chaque machine, à des instants aléatoires, mène à une cession ou à une relève, puis à une relance locale. | C | T |
| ROB-08 | Une machine lente (processeur volé un tick sur trois) est destituée si elle manque p+1 ticks de suite. Jamais de double commande. | C | T |
| ROB-09 | Aucun analyseur de trames ne plante, quelle que soit l'entrée : 24 h de test à données aléatoires. | X | T |
| ROB-10 | Les deux machines mortes puis redémarrées ensemble : sortie de la loi de secours sans à-coup. | X | T |

### 5.10 Sécurité

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| SEC-01 | Une trame malformée, de version inconnue ou d'émetteur inconnu est rejetée et comptée, sans effet sur le service. | S | T |
| SEC-02 | Chaque trame porte un authentifiant calculé avec une clé propre au lien. Une trame forgée sans la clé est rejetée. | C | T |
| SEC-03 | Le rejeu d'une trame authentique capturée est rejeté par l'étiquette de tick. | C | T |
| SEC-04 | Aucune machine ne peut s'attribuer un mandat. Un `ORDRE` portant un mandat futur est rejeté et signalé. | C | T |
| SEC-05 | Une politique de sécurité QNX attribue un type à chaque processus : seul `relais-net` ouvre des sockets, seul `relais-core` sert `/dev/relais`. Les commandes de test sont absentes de l'image de production. | C | T |
| SEC-06 | Les demandes de mandat sont limitées en débit. Une avalanche de demandes ne retarde pas le traitement des ordres. | C | T |
| SEC-07 | Les clés sont renouvelées à chaque démarrage par un échange authentifié. | X | T |

### 5.11 Observabilité

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| OBS-01 | Les processus écrivent dans le journal système de QNX (slog2). | S | D |
| OBS-02 | `/dev/relais/status` donne l'état en texte : rôle, mandat, âge de la réplique, compteurs, témoin. | S | D |
| OBS-03 | Des événements utilisateur sont insérés dans la trace noyau : `STATUT` reçu, ordre émis, changement de rôle, cession. | S | M |
| OBS-04 | Le banc capture chaque lien dans un fichier pcap. | S | T |
| OBS-05 | Un rapport de campagne est généré en HTML autonome : tableau par type de panne, histogrammes, pires cas, courbes superposées au jumeau sans panne. | S | D |
| OBS-06 | Un tableau de bord en direct montre qui tient le mandat, la source de chaque tick, la commande et la position. | C | D |
| OBS-07 | `relais-verify` relit un journal d'actionneur et contrôle tous les invariants hors ligne. | C | T |
| OBS-08 | Le journal s'exporte au format MCAP, pour lecture dans un visualiseur standard. | X | D |

### 5.12 Banc d'injection de pannes

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| INJ-01 | Une seule commande lance les deux machines QNX et l'actionneur, sans droits d'administrateur sur le PC. | S | D |
| INJ-02 | Mort : arrêt brutal d'une machine à un instant choisi par rapport au tick. C'est le banc, qui connaît le tempo, qui déclenche. | S | T |
| INJ-03 | Gel et dégel d'une machine, durée paramétrable. | S | T |
| INJ-04 | Coupure et rétablissement d'un lien, avec et sans perte de porteuse. | S | T |
| INJ-05 | Câble programmé entre calculateurs : perte, retard, duplication, désordre, par trame, par type de message et par sens. | S | T |
| INJ-06 | Chaque essai est décrit par un fichier (scénario, pannes, instants, graine) et se rejoue. Le rapport cite ce fichier. | S | T |
| INJ-07 | Points de mort : la machine s'éteint à un point précis du code (section 7.5) au tick demandé. | C | T |
| INJ-08 | Balayage : 100 instants par période, pour chaque type de panne, chaque rôle et chaque phase du protocole. | C | T |
| INJ-09 | Machine lente : vol de processeur par gels brefs répétés, et par une tâche prioritaire dans l'invité. | C | T |
| INJ-10 | Faute de valeur : une commande de test ajoute un biais aux ordres d'une machine. | C | T |
| INJ-11 | Mutants : des variantes du logiciel à défaut volontaire (section 7.6) passent la même campagne, qui doit signaler chacune. | C | T |
| INJ-12 | Recherche automatique du pire cas : un optimiseur choisit instants et durées de panne pour maximiser l'écart au jumeau sans panne. | X | T |
| INJ-13 | Trois bancs tournent en parallèle sur le même PC, et leurs résultats sont fusionnés. | X | T |

### 5.13 Usage des technologies propres à QNX

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| QNX-01 | `relais-core` est un gestionnaire de ressources ; son interface se réduit à `open`, `read` et `devctl`. | S | I |
| QNX-02 | Les temporisations et les notifications entre processus utilisent des impulsions. Aucune attente active. | S | I |
| QNX-03 | Les processus critiques tournent en priorité fixe et verrouillent leur mémoire au démarrage. | S | I |
| QNX-04 | Une trace noyau montre le chemin du `STATUT` à l'ordre et la priorité effective de chaque étape, pile réseau comprise. | S | D |
| QNX-05 | La mort d'un processus est connue de ses correspondants par la fermeture de sa connexion, sans interrogation périodique. | C | T |
| QNX-06 | Les processus sont lancés et relancés par le service de supervision de QNX. | C | D |
| QNX-07 | La politique de sécurité est appliquée, et un refus est montré. | C | D |
| QNX-08 | Le délai entre le lancement du noyau et l'état de second certifié est mesuré et publié. | C | M |
| QNX-09 | Les deux calculateurs sont deux invités de l'hyperviseur QNX sur une même carte, ou deux cartes réelles. | X | D |

---

## 6. Protocole et formats

### 6.1 Trame

Toutes les trames sont des datagrammes UDP de taille fixe par type, 512 octets au plus.

| Champ | Taille | Rôle |
|---|---|---|
| Nombre magique | 4 | Reconnaître le protocole |
| Version | 1 | Refuser une version inconnue |
| Type | 1 | Voir 6.2 |
| Émetteur | 1 | A, B, troisième machine, actionneur |
| Drapeaux | 1 | Prêt, cession, trame de test |
| Tick | 8 | Tick émis (`STATUT`) ou tick visé (ordres) |
| Mandat | 4 | Mandat courant (`STATUT`) ou mandat cru par l'émetteur |
| Longueur utile | 2 | Taille du contenu |
| Réservé | 2 | Zéro |
| Contenu | variable | Selon le type |
| Contrôle | 8 | CRC-32C au socle ; authentifiant à clé au niveau complet |

### 6.2 Messages

| Type | Sens | Quand | Contenu utile |
|---|---|---|---|
| `STATUT` | Actionneur → A et B | Chaque tick | Titulaire, source de la valeur appliquée, manques consécutifs, second certifié ou non, témoin et cause, position et vitesse mesurées, valeur appliquée, consigne du volant simulé |
| `ORDRE` | Titulaire → actionneur | Chaque tick | Valeur pour le tick visé, testament de N valeurs |
| `OMBRE` | Second → actionneur | Chaque tick (complet) | Valeur pour le tick visé, âge de la réplique |
| `DEMANDE` | Non titulaire → actionneur | Sur soupçon (socle) | Dernier mandat vu, raison, premier ordre facultatif |
| `CESSION` | Titulaire → actionneur et second | Panne locale, maintenance | Mandat cédé, raison |
| `ETAT` | Titulaire → second | Chaque tick | État complet de la loi de commande, dernière valeur émise |
| `SUIVI` | Second → titulaire | Chaque tick | Âge de la réplique, drapeau prêt |

### 6.3 Paramètres

| Paramètre | Défaut | Sens |
|---|---|---|
| T | 5 ms | Période du tick |
| p | 1 | Manques consécutifs tolérés sans destitution |
| N | 8 ticks | Horizon du testament |
| Certification | 3 ticks | Ticks concordants pour certifier un second |
| Bail | 4 ticks | Silence de l'actionneur après lequel une machine se destitue |
| ε | 0,5 % de la pleine échelle | Écart toléré entre ordre et ombre |
| δ | 2 % de la pleine échelle | Écart toléré à la relève |
| Durée dégradée | 30 s | Temps admis sans redondance avant « arrêt demandé » |

Règle de cohérence, vérifiée au démarrage : N ≥ p + 2 + 3, pour que le testament couvre la relève même si trois demandes se perdent.

### 6.4 Journal de l'actionneur

Le journal de l'actionneur fait foi : c'est lui que lit le vérificateur.

- **Un enregistrement par tick** : numéro de tick, horodatage, mandat, titulaire, source de la valeur (titulaire, suppléance, testament, secours), valeur appliquée, position, manques consécutifs, témoin.
- **Un enregistrement par événement** : changement de mandat, certification accordée ou retirée, demande accordée ou refusée, ordre écarté avec sa raison (émetteur, mandat, tick, contrôle, vraisemblance), alerte de désaccord, entrée et sortie de la loi de secours.
- **Format** : une ligne JSON par enregistrement au socle ; binaire à taille fixe au niveau complet, avec conversion.

---

## 7. Plan de validation

### 7.1 Cinq niveaux d'essai

| Niveau | Où | Ce qui est testé |
|---|---|---|
| 0 — unitaire | PC | Trames, arbitre, machine d'états, loi de commande |
| 1 — exploration exhaustive | PC | Tous les entrelacements bornés du cœur, pannes comprises |
| 2 — simulation à graine | PC | Longues séquences aléatoires, avec un modèle de temps |
| 3 — pannes réelles | QEMU | La chaîne complète : machines tuées, figées, câbles coupés |
| 4 — matériel | Deux cartes | Latences réelles (niveau extrême) |

### 7.2 Invariants

| ID | Invariant |
|---|---|
| I1 | **Unicité.** À chaque tick, l'actionneur applique une seule valeur, et sa source est le titulaire, le second certifié en suppléance, le testament ou la loi de secours. Rien d'autre. |
| I2 | **Monotonie.** Le numéro de mandat ne décroît jamais et change au plus une fois par tick. Deux machines ne sont jamais titulaires du même mandat. |
| I3 | **Pas de revenant.** Aucun ordre visant un autre tick que le prochain n'est appliqué, et aucun ordre d'une machine destituée n'est appliqué après sa destitution. |
| I4 | **Trou borné.** Sous l'hypothèse de panne simple, le nombre de ticks consécutifs sans ordre frais ne dépasse pas p+1 au socle, et vaut zéro avec un second certifié. |
| I5 | **Continuité.** L'écart entre la valeur appliquée et celle du jumeau sans panne reste sous δ. |
| I6 | **Testament.** Aucune valeur de testament n'est appliquée au-delà de son horizon. À son épuisement, la loi de secours s'applique au tick suivant. |
| I7 | **Pas de relève à tort.** Sans panne injectée, y compris sous surcharge de priorité inférieure et avec la perte de p ordres consécutifs, le titulaire ne change pas. |
| I8 | **Double panne.** Si aucune machine n'est en état, l'actionneur est en loi de secours au plus N+1 ticks après le dernier ordre frais. |
| I9 | **Réintégration.** Pendant qu'une machine rejoint, chaque tick est servi par le titulaire. |
| I10 | **Signalisation.** Toute perte de redondance apparaît dans le témoin au plus 2 ticks après sa détection, et y reste tant qu'elle dure. |
| I11 | **Progrès.** Chaque machine d'états traite tout événement en temps borné, et n'a aucun état sans issue. |

### 7.3 Exploration exhaustive du cœur

1. Les trois acteurs (A, B, actionneur) sont trois machines d'états pures, compilées pour le PC à partir des fichiers de la cible.
2. L'état global réunit leurs trois états, les trames en vol sur chaque lien et les pannes en cours.
3. Dans chaque état, l'explorateur énumère tous les événements possibles : livrer une trame en vol, la perdre, la dupliquer, faire battre le tick, tuer une machine, la figer, la dégeler, la redémarrer, couper ou rétablir un demi-lien.
4. Le parcours garde la table des états déjà vus et s'arrête à une profondeur de quelques ticks. Budget de pannes : une au socle, deux au niveau complet.
5. Les invariants de sûreté (I1, I2, I3, I6) sont contrôlés dans chaque état. Le trou borné (I4) est contrôlé sur chaque chemin où l'adversaire respecte l'hypothèse de panne simple.
6. Tout contre-exemple est écrit comme un scénario que le simulateur et le banc QEMU savent rejouer.

Ordres de grandeur visés, à confirmer par la première version : 100 000 états distincts au socle, dix millions au niveau complet.

### 7.4 Simulation à graine

Le même cœur tourne dans un simulateur à événements discrets. Une graine fixe tous les tirages : délais des trames (en fractions de tick), pertes, instants et durées de panne. Le simulateur avance de plusieurs millions de ticks par seconde. Une graine qui viole un invariant est réduite au plus petit scénario fautif, puis gardée comme test de non-régression.

Le simulateur produit aussi le **jumeau sans panne** de chaque scénario.

### 7.5 Campagne de pannes réelles sous QEMU

- **Balayage de l'instant.** Pour chaque panne (mort, gel, coupure), l'instant est déplacé de 0 à T par pas de T/100 par rapport au tick.
- **Points de mort.** La machine s'éteint à un point précis du code :

| Point | Position |
|---|---|
| P1 | `STATUT` reçu, avant le calcul |
| P2 | Calcul fait, avant tout envoi |
| P3 | `ORDRE` émis, `ETAT` pas encore |
| P4 | `ETAT` émis sur un lien, pas sur l'autre |
| P5 | Titulaire : `CESSION` émise vers l'actionneur seulement, ou vers le second seulement |
| P6 | Second : soupçon levé, `DEMANDE` pas encore émise |
| P7 | Second : mandat reçu, premier `ORDRE` pas encore émis |
| P8 | Machine destituée : pendant son passage au rôle de second |
| P9 | Réintégration : avant la certification |

- **Gels.** Durées d'un tick à dix minutes, en progression géométrique, dans les deux rôles.
- **Câbles.** Chacun des six demi-liens seul, puis par paires au niveau complet.
- **Pannes imbriquées.** Une seconde panne pendant le traitement de la première : l'issue doit être un état prévu, ou la loi de secours.
- **Surcharge.** Trente minutes à douze heures sans panne, sous charge, pour I7.

**Limites connues.** Les deux machines partagent le PC : un pic de charge de l'hôte touche les deux. Tuer QEMU n'est pas couper le courant d'un calculateur. Le banc ne produit aucune faute matérielle fine (bit inversé en mémoire).

### 7.6 Les mutants : prouver que le banc voit les fautes

Chaque mutant est une variante du logiciel avec un défaut volontaire. Il passe la même campagne. Si le banc ne le signale pas, c'est le banc qui est en défaut.

| Mutant | Défaut introduit | Ce que le banc doit signaler |
|---|---|---|
| M1 | L'actionneur accepte l'ordre de n'importe quelle machine | I1 |
| M2 | L'actionneur ignore l'étiquette de tick | I3 |
| M3 | L'actionneur accorde le mandat sans constater le silence | I7 |
| M4 | Le testament est joué au-delà de son horizon | I6 |
| M5 | Le second ne remplace pas son état par celui reçu | I5 |
| M6 | Le numéro de mandat repart de zéro au redémarrage de l'actionneur | I2 |
| M7 | Le second émet des `ORDRE` sur simple soupçon, sans attendre l'accord | Ordres d'un non-titulaire relevés au journal (MAN-08) ; l'actionneur, lui, doit tenir |
| M8 | Le témoin s'éteint au retour d'un lien alors que le second n'est pas certifié | I10 |

Les mutants M1 à M4 ne touchent que des exigences du socle. M7 vérifie la défense en profondeur : le défaut est dans une machine, et l'arbitre doit l'absorber sans qu'aucun invariant de l'actionneur ne tombe.

### 7.7 Mesures

- **Intervalle entre ordres frais** : relevé sur le journal de l'actionneur, en ticks et en temps.
- **Écart au jumeau sans panne** : valeur appliquée et position, tick par tick.
- **Temps de traitement dans une machine** : par les événements de trace noyau (OBS-03).
- **Résultats publiés** : histogramme, médiane, 99e et 99,9e centiles, maximum, et les trois pires cas commentés.

### 7.8 Seuils de réussite par niveau

| Mesure | Socle | Complet | Extrême |
|---|---|---|---|
| États explorés du cœur | 100 000, une panne | 10 000 000, deux pannes | Idem, plus une spécification indépendante concordante |
| Ticks simulés à graine | 1 million | 1 milliard | — |
| Morts réelles | 200 | 2 000 balayées, plus tous les points de mort | 10 000 |
| Gels et dégels | 2 000 | 10 000 | 100 000 |
| Coupures de lien | 500 | 5 000, sens uniques compris | — |
| Mutants signalés | 4 sur 4 (M1 à M4) | 8 sur 8 | — |
| Durée sans relève à tort | 30 min | 12 h sous charge | 72 h |
| Violations d'invariant tolérées | 0 | 0 | 0 |

Toute violation rencontrée en cours de développement est conservée comme test de non-régression, avec le scénario qui la reproduit.

---

## 8. Démonstration (vidéo de moins de trois minutes)

| Temps | À l'écran | Message |
|---|---|---|
| 0:00 – 0:20 | Une phrase et un schéma | Sans colonne de direction, un calculateur qui se tait n'est plus un état sûr. Il en faut deux, et un seul doit commander. |
| 0:20 – 0:45 | Schéma d'architecture | Deux QNX, un actionneur arbitre qui donne le tempo, un testament dans chaque ordre. |
| 0:45 – 1:20 | Courbe en direct et terminal | Slalom en cours. On tue la machine A. La courbe ne bouge pas. Bandeau : « mandat 1 → 2, zéro tick sans ordre frais ». Témoin jaune. |
| 1:20 – 1:45 | Idem | A redémarre et redevient second certifié : témoin éteint. On fige B 200 ms puis on la dégèle : « ordre écarté : mandat périmé, tick périmé ». B se range second. |
| 1:45 – 2:05 | Idem | On coupe le câble entre les calculateurs : aucune relève, témoin jaune « lien perdu ». |
| 2:05 – 2:30 | Rapport de campagne | Nombre de pannes balayées sur tous les instants du cycle, zéro violation ; états explorés ; mutants : 8 sur 8 signalés. |
| 2:30 – 2:45 | Trace noyau et terminal | Le chemin du `STATUT` à l'ordre, avec les priorités. Un processus intrus se voit refuser `/dev/relais`. |
| 2:45 – 3:00 | Dépôt | Licence, commande unique pour rejouer la démo, tutoriel. |

**Scénarios.** Trois sources, par ordre de repli :

1. Un générateur à graine fixe : slalom, changement de voie, échelon, avec perturbation de route. Toujours disponible.
2. Un profil de freinage (CMD-08).
3. Des enregistrements réels d'angle de volant tirés d'un jeu de données de conduite public (licence à confirmer).

---

## 9. Livrables et dépôt

```
relais/
├── README.md              anglais : pitch, démo en une commande, preuves d'usage de QNX
├── LICENSE                Apache-2.0
├── core/                  bibliothèque C portable : arbitre, machine d'états, trames
├── target/                processus QNX : relais-net, relais-core, relais-ctl, relaisctl
├── actuator/              arbitre + modèle de crémaillère, pour PC et pour QNX
├── image/                 fragments pour mkqnximage, politique de sécurité, identité par adresse MAC
├── host/
│   ├── bench/             lancement des machines, orchestrateur, câble programmé
│   ├── explore/           exploration exhaustive du cœur
│   ├── simulate/          simulation à graine, jumeau sans panne
│   ├── verify/            relais-verify : invariants sur journal
│   ├── mutants/           variantes à défaut volontaire
│   └── report/            rapport HTML, courbes
├── scenarios/             trajectoires de référence, plans de pannes
├── docs/                  ce cahier des charges, protocole, table de vérité, résultats de campagne
└── codelab/               tutoriel pas à pas
```

- **Langages** : C11 pour le cœur, les processus QNX et l'actionneur ; Python 3 sans dépendance externe pour le banc et le rapport.
- **Instructions de construction** : version de QNX, cible (QEMU x86_64), chaîne de compilation, comme le règlement du hackathon l'exige.
- **Preuves d'usage de QNX** à pointer dans la description : gestionnaire de ressources, passage de messages et héritage de priorité, impulsions, politique de sécurité, supervision, trace noyau.
- **Document de sûreté** court, calqué sur l'annexe 6 §3.4.3.2 : principe du basculement, logique, niveau de redondance, contrôles intégrés, limites d'efficacité.
- **Tutoriel** candidat au bonus : « Deux machines QNX, un seul maître : détecter une panne en deux périodes sans se tromper de chef ».
- **Tout en anglais** dans le dépôt et la vidéo.

---

## 10. Planning

Trente-quatre jours séparent le mercredi 11 novembre du mardi 15 décembre.

| Semaine | Dates | Contenu | Jalon |
|---|---|---|---|
| 1 | 11 – 17 nov. | Essais de faisabilité E1 à E6. Cœur pur et tests unitaires. Explorateur, première version. Actionneur simulé et câble programmé sur PC. | L'exploration passe sur PC |
| 2 | 18 – 24 nov. | Processus QNX. Première relève de bout en bout sous QEMU : on tue A, B reprend. | Une relève réussie et mesurée |
| 3 | 25 nov. – 1er déc. | Gel et dégel, coupures de câble, réintégration, testament. Orchestrateur et première campagne. | **Socle atteint** : 200 morts, 2 000 gels, zéro violation |
| 4 | 2 – 8 déc. | Ombres et suppléance, certification, comparaison ordre-ombre. Points de mort, mutants, simulation à graine. Politique de sécurité, traces. | **Complet atteint** |
| 5 | 9 – 14 déc. | README, tutoriel, vidéo, rapport de campagne. Un seul élément extrême au choix. Soumission le 14 au soir. | Soumis, un jour de marge |

**Essais de faisabilité (trois premiers jours).**

| Essai | Question | Si la réponse est non |
|---|---|---|
| E1 | Deux machines QNX issues de la même image, reliées par un lien `-netdev dgram` à travers un relais sur le PC, échangent-elles des datagrammes UDP sans droits d'administrateur ? | Lien en multidiffusion (`-netdev socket,mcast`) ; à défaut, chaque machine passe par le réseau utilisateur de QEMU et le relais tourne sur le PC |
| E2 | Quel est l'aller-retour UDP entre une machine et un programme du PC, en médiane et au pire, avec KVM ? | Allonger la période à 10 ou 20 ms ; placer l'actionneur dans une troisième machine |
| E3 | Après `stop` puis `cont`, que voit l'horloge de l'invité ? La machine sait-elle qu'elle a dormi ? | Aucun repli nécessaire : le protocole est sûr dans les deux cas. La réponse fixe seulement le récit de la démonstration |
| E4 | Un processus QNX obtient-il l'accès aux ports d'entrée-sortie et éteint-il la machine par `isa-debug-exit` ? | Mort déclenchée de l'extérieur à l'instant voulu, sans point de code exact |
| E5 | Dans l'invité, quel est le délai du `STATUT` reçu à l'ordre émis sous charge, et à quelle priorité la pile réseau traite-t-elle les trames ? | Régler les priorités de la pile réseau ; allonger la période |
| E6 | Combien de temps du lancement de QEMU à une machine prête ? | Alléger l'image ; restaurer un instantané au lieu de redémarrer |

**Ce qui saute en premier en cas de retard** : l'actionneur dans une troisième machine, l'authentifiant des trames, le tableau de bord en direct, le profil de freinage. Le socle, l'exploration exhaustive et les mutants M1 à M4 ne sautent pas.

---

## 11. Risques

| Risque | Gravité | Parade |
|---|---|---|
| Le PC introduit des retards qui font manquer des ticks sans panne | Forte | Essais E2 et E5 dès le premier jour ; processeurs réservés à chaque machine ; mode ralenti pour les campagnes de logique ; mesures de temps publiées à part, avec leur distribution |
| Le lien direct entre deux QEMU ne fonctionne pas comme prévu avec QNX | Forte | Essai E1 ; deux solutions de repli décrites |
| « L'actionneur est un point unique de panne » | Moyenne | Le dire dans le périmètre ; logique d'arbitrage minuscule et explorée en entier ; deux voies au niveau extrême |
| Le projet est perçu comme un simple battement de cœur avec bascule | Moyenne | Mettre en avant l'arbitre, le revenant, l'exploration et les mutants ; la vidéo montre le gel et la coupure de câble, pas seulement la mort |
| Les durées sous QEMU sont contestées | Moyenne | Mention « émulé » ; la sûreté ne dépend pas du temps ; cartes réelles si disponibles |
| Le périmètre déborde | Forte | Niveaux S, C, X ; jalon socle en fin de semaine 3 |
| Un défaut commun aux deux machines les tue ensemble | Moyenne | Hors périmètre, annoncé ; loi de secours testée ; doublure dissemblable au niveau extrême |
| Une revendication réglementaire est jugée abusive | Moyenne | Formulation « aligné sur », jamais « conforme » |
| Un projet voisin apparaît avant la date limite | Faible | Consulter la galerie avant de soumettre ; différenciation de la section 2 |
| Ce projet s'ajoute à d'autres soumissions menées en parallèle | Forte | Réutiliser l'outillage déjà écrit (lancement et arrêt de QEMU en boucle, exploration d'états) ; vérifier dans le règlement que des soumissions qui partagent de l'outillage restent distinctes |
| L'accès aux ports d'entrée-sortie est refusé à un processus ordinaire | Faible | Points de mort réservés à une image de test ; repli par arrêt externe |

---

## 12. Pousser l'idée au bout

Au-delà du niveau complet, par ordre d'intérêt pour un jury :

1. **Recherche automatique du pire scénario.** Un optimiseur pilote le banc : il choisit les instants, les durées et les combinaisons de pannes pour maximiser l'écart au jumeau sans panne. Le rapport publie le pire cas trouvé et le temps de recherche. C'est l'usage le plus défendable de l'IA dans ce projet.
2. **Trois machines et vote.** Avec un troisième calculateur, l'actionneur applique la valeur sur laquelle deux machines s'accordent : une machine qui calcule faux est démasquée, pas seulement détectée.
3. **Spécification formelle indépendante.** Le protocole est décrit dans un langage de spécification (TLA+ par exemple) et vérifié par un explorateur de modèles. Les deux explorations doivent s'accorder sur les invariants.
4. **Doublure dissemblable.** Le second fait tourner une loi de commande plus simple, écrite séparément. Elle couvre le défaut logiciel commun, au prix d'un écart admis à la relève.
5. **Deux voies d'actionneur.** Chaque voie a son arbitre, et la perte d'une voie est tolérée. L'actionneur cesse d'être un point unique de panne.
6. **Mise à jour sans interruption.** On met à jour le second, on lui cède le mandat, on met à jour l'autre. Aucun tick sans ordre frais.
7. **Matériel réel.** Deux cartes Raspberry Pi reliées par Ethernet, une troisième pour l'actionneur. Les pannes deviennent des coupures de courant par relais.
8. **Hyperviseur.** Les deux calculateurs sont deux invités QNX sur une même carte, à côté d'un invité qui joue l'info-divertissement et qu'on fait planter.
9. **Journal des relèves pour l'après-accident.** Chaque changement de mandat est remis à un enregistreur de données d'événement, avec sa cause et son délai.
10. **Dossier de sûreté réduit.** Analyse des modes de défaillance générée à partir de la table de vérité, et matrice de traçabilité des exigences vers les tests.
11. **Rejeu d'une panne à l'identique.** Enregistrement et rejeu de QEMU sur une machine, pour revoir instruction par instruction le pire cas trouvé.
12. **Clés renouvelées à chaque démarrage** et rotation en marche, sans perte de tick.

---

## 13. Critères d'acceptation

**Socle — soumission possible.**

- [ ] Toutes les exigences de niveau S sont vérifiées.
- [ ] Une seule commande rejoue la démonstration sous QEMU, sans droits d'administrateur.
- [ ] 100 000 états du cœur explorés, zéro violation.
- [ ] 200 morts, 2 000 gels et 500 coupures de lien réels, zéro violation.
- [ ] Sur relève après mort, jamais plus de p+2 ticks entre deux ordres frais.
- [ ] Les mutants M1 à M4 sont signalés.
- [ ] Trente minutes sous charge sans relève à tort.
- [ ] Dépôt public, licence, README en anglais, vidéo de moins de trois minutes.

**Complet — version visée.**

- [ ] Toutes les exigences de niveau C sont vérifiées.
- [ ] Dix millions d'états explorés avec deux pannes ; un milliard de ticks simulés.
- [ ] 2 000 morts balayées, tous les points de mort, 10 000 gels, 5 000 coupures : zéro tick sans ordre frais avec un second certifié.
- [ ] Les huit mutants sont signalés.
- [ ] Douze heures sous charge sans relève à tort ; trois pires cas expliqués par une trace.
- [ ] Un processus intrus est refusé par la politique de sécurité.
- [ ] Document de sûreté et tutoriel publiés.

**Extrême.**

- [ ] Au moins deux éléments de la section 12 réalisés et mesurés.

---

## 14. Ce qui est vérifié et ce qui reste à valider

| Affirmation | Statut |
|---|---|
| Paragraphes du règlement n° 79 cités en 1.2 | Lus le 11 novembre 2026 sur une copie PDF de la révision 4 (7 novembre 2018, série 03 d'amendements) hébergée par un site tiers. Des amendements postérieurs existent, et un groupe de travail de l'ONU discute en 2025-2026 de précisions sur la direction sans liaison mécanique. **À relire sur le texte en vigueur.** |
| La direction sans liaison mécanique est homologable sous le nom de *full-power steering* depuis 2003, et relève de l'annexe 6 | D'après l'agence britannique d'homologation, par un résumé de recherche ; page non lue en entier |
| Définitions ISO 26262 (FTTI, FDTI, FRTI, EOTTI) | Sources secondaires concordantes ; la norme n'a pas été consultée |
| Infiniti Q50 et Tesla Cybertruck | Presse spécialisée et Wikipédia ; aucun document de constructeur |
| Options de QEMU de la section 4.11 | Essayées sur ce PC avec QEMU 8.2.2 : la ligne de commande complète est acceptée, les commandes `stop`, `cont`, `quit`, `system_reset` et `set_link` existent, `set_link` a réussi sur une machine à l'arrêt. **Aucun échange de trames entre deux invités n'a été essayé.** |
| KVM disponible | Le périphérique est présent sur ce PC ; non essayé avec QNX |
| Contenu des dépôts voisins | Lus le 11 novembre 2026 ; ils évoluent. `qnx-hotswap` n'est connu que par sa description |
| Aucun projet public de relève entre deux machines QNX | Recherche sur les noms et descriptions de dépôts GitHub seulement, pas sur leur contenu ni sur les dépôts privés |
| Accès aux ports d'entrée-sortie sous QNX 8.0 (`ThreadCtl`, niveau d'entrée-sortie, capacité requise) | Vérifié dans la documentation QNX 8.0 ; non essayé (E4) |
| Événements utilisateur dans la trace noyau (`trace_logf`) | Vérifié dans la documentation QNX 8.0 |
| Pilotes réseau `devs-vtnet_pci.so` et `devs-em.so` pour `io-sock` | Cités dans la documentation QNX 8.0 ; correspondance avec les cartes émulées par QEMU non confirmée (E1) |
| Deux machines QNX échangent des trames par un lien `dgram` | **Hypothèse**, essai E1 |
| Délai Δ d'au plus 1 ms, calcul en 0,5 ms, gigue d'au plus 1 ms | **Hypothèses**, essais E2 et E5 |
| Période de 5 ms tenable sous QEMU | **Hypothèse**, essais E2 et E5 |
| Comportement de l'horloge de l'invité après un gel | Inconnu, essai E3 ; le protocole n'en dépend pas |
| Héritage de priorité jusqu'à la pile réseau | **Hypothèse** fondée sur le fonctionnement général de QNX, à montrer par trace (QNX-04) |
| Bornes de relève (3 périodes au socle, zéro tick au complet) | Raisonnement sur le protocole, section 4.5 ; **cibles**, à mesurer |
| Tailles d'exploration (100 000 et dix millions d'états) | Ordres de grandeur estimés, non mesurés |
| Calcul identique au bit près sur les deux machines | **Hypothèse** (même binaire, même processeur virtuel), contrôlée par DET-09 |
| Service de supervision et politique de sécurité sous QNX 8.0 | Vus dans des dépôts concurrents, à confirmer dans la documentation |
| Gestionnaires de grappes, adresses flottantes, COLO, bus à déclenchement temporel, calculateurs de vol | Cités de mémoire comme repères, non revérifiés |
| Jeton croissant refusé par la ressource quand il est périmé | Principe décrit par M. Kleppmann en 2016 ; article localisé, lu par résumé |

---

## 15. Glossaire

| Terme | Sens |
|---|---|
| Sûr à l'arrêt, sûr en marche | Système qui devient sûr en se taisant ; système qui doit continuer de fonctionner pour rester sûr |
| Tick | Battement périodique donné par l'actionneur ; unité de temps du protocole |
| Mandat | Droit de commander, numéroté par l'actionneur |
| Titulaire | Machine qui détient le mandat courant |
| Second | Machine qui suit le titulaire, prête à le remplacer |
| Certifié prêt | Second dont les ombres concordent avec les ordres, constaté par l'actionneur |
| Ordre frais | Ordre calculé à partir du `STATUT` du tick précédent |
| Ombre | Ordre calculé par le second, envoyé à l'actionneur sans mandat |
| Suppléance | Tick servi par l'ombre du second parce que l'ordre du titulaire manque |
| Testament | Valeurs prévues pour les ticks suivants, embarquées dans chaque ordre |
| Loi de secours | Commande locale de l'actionneur quand il n'a plus ni ordre ni testament |
| Relève | Passage du mandat d'une machine à l'autre |
| Cession | Abandon volontaire du mandat par son titulaire |
| Destituée | Machine à qui l'actionneur a retiré le mandat |
| Revenant | Machine qui reprend après un gel en se croyant encore titulaire |
| Double commande | Deux machines dont les ordres seraient appliqués en même temps ; ce que le projet rend impossible |
| Jumeau sans panne | Le même scénario rejoué sans panne, qui sert de référence |
| Câble programmé | Relais logiciel qui perd, retarde ou désordonne les trames à la demande |
| Point de mort | Endroit du code où la machine s'éteint sur commande, pour les essais |
| Mutant | Variante du logiciel à défaut volontaire, qui sert à tester le banc |
| Témoin | Signal d'avertissement : éteint, jaune ou rouge |
| Gestionnaire de ressources | Processus QNX qui apparaît comme un fichier ou un répertoire, par exemple sous `/dev` |
| Impulsion | Petit message QNX non bloquant, utilisé pour les notifications |
| Politique de sécurité | Règles QNX qui fixent quels types de processus peuvent se parler |

---

## 16. Sources

**Réglementation et normes**

- Règlement ONU n° 79, révision 4 (document E/ECE/324/Rev.1/Add.78/Rev.4), copie consultée : https://villanyautosok.hu/wp-content/uploads/2019/05/r079r4e.pdf
- Travaux en cours sur le règlement n° 79 : https://globalautoregs.com/rules/90/documents?year=2025
- Agence britannique d'homologation, direction sans liaison mécanique : https://www.vehicle-certification-agency.gov.uk/blog/assessing-the-compliance-of-steer-by-wire-systems-for-individual-vehicle-approval
- ISO 26262, définitions (source secondaire) : https://www.arteris.com/blog/critical-safety-overview-and-definitions/
- ISO 26262, intervalle de tolérance au défaut (article SAE 2024-01-2791) : https://saemobilus.sae.org/downloads/papers/2024-01-2791/Full%20Text%20PDF

**Exemples de série**

- Direction sans liaison mécanique : https://www.wikipedia.org/wiki/Steer-by-wire
- Redondance du Cybertruck, démontage : https://theautopian.com/heres-the-fascinating-redundancy-manufacturing-experts-found-when-they-tore-into-the-tesla-cybertrucks-steer-by-wire-system

**QNX**

- Privilèges d'entrée-sortie des fils : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.prog/topic/process_thread_IO_privileges.html
- `ThreadCtl()` : https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/threadctl.html
- `trace_logf()` : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/trace_logf.html
- Horloges et temporisateurs : https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/kernel_CLOCKANDTIMER.html
- Démarrage d'`io-sock` et pilotes : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.io_sock/topic/start_io-sock.html
- Pilotes réseau pour `io-sock` : https://qnx.com/developers/docs/8.0/com.qnx.doc.qnxsdp.migration/topic/bsp_support.html
- Bibliothèque de reprise côté client (QNX 7.1) : https://www.qnx.com/developers/docs/7.1/com.qnx.doc.ham/topic/client_funcs.html

**QEMU**

- Options de lancement : https://www.qemu.org/docs/master/system/invocation.html
- Commandes de pilotage (QMP) : https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html
- Enregistrement et rejeu : https://www.qemu.org/docs/master/system/replay.html

**Principes**

- M. Kleppmann, « How to do distributed locking » (2016) : https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html

**Hackathon et projets voisins**

- Page et règlement : https://qnx.devpost.com/ et https://qnx.devpost.com/rules
- https://github.com/TheUndefinedEngineer/Medical_Infusion_Pump
- https://github.com/AhmedAdelWafdy7/autoware-qnx-guardian
- https://github.com/zuhairhalawa/faultline
- https://github.com/Lainholic/qnx-safepulse-lab
- https://github.com/atss0/qnx-hotswap
