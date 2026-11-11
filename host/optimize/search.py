#!/usr/bin/env python3
"""Relais — generic black-box search over a mixed space (INJ-12, spec 12 #1).

The adversarial worst-case search of host/optimize/worstcase.py needs two
things: an evolutionary loop driving a population, and a fallback that polishes
a single point when the evolution stalls. Both live here and both are pure: they
only know a Space (a box of real genes) and an `evaluate` callable mapping a
genome to a Candidate. Nothing here talks to the simulator, so the search itself
is unit-testable against an objective whose optimum is known
(host/optimize/test_worstcase.py).

Budget is counted in evaluations AND in seconds, and is never exceeded: the
caller pays one simulator run per evaluation, so the counter is the contract.
"""
from __future__ import annotations

import math
import random
import statistics
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

Genome = Tuple[float, ...]


@dataclass
class Candidate:
    """One evaluated point: its genome and its fitness (higher = worse for the
    system under test, i.e. better for the adversary)."""

    genome: Genome
    fitness: float
    counter_example: bool = False
    error: bool = False
    payload: Any = None

    def outranks(self, other: "Candidate") -> bool:
        """Ranking used everywhere: a counter-example (an invariant violation)
        beats any score, a failed evaluation loses to everything, then the
        higher fitness wins. A violation is a proof, not a number."""
        if self.error != other.error:
            return other.error
        if self.counter_example != other.counter_example:
            return self.counter_example
        return self.fitness > other.fitness


@dataclass
class SearchResult:
    """What one search consumed and what it found."""

    best: Optional[Candidate]
    evaluations: int
    generations: int
    seconds: float
    algorithm: str
    stalled: bool = False
    history: List[Dict[str, Any]] = field(default_factory=list)


class Space:
    """A box of `dim` real genes, each inside [lo, hi] (normalised 0..1).

    The worst-case genome mixes continuous quantities (fault instant, duration)
    and categorical ones (fault kind, target machine, link direction). Both are
    carried as real genes and a categorical one is decoded by quantisation, so a
    single Gaussian mutation and a single crossover serve the whole genome.
    """

    def __init__(self, dim: int, lo: float = 0.0, hi: float = 1.0) -> None:
        if dim <= 0:
            raise ValueError("a space needs at least one gene")
        if hi <= lo:
            raise ValueError("empty box: hi must be above lo")
        self.dim = dim
        self.lo = lo
        self.hi = hi

    def clamp(self, genome: Sequence[float]) -> Genome:
        """Bring every gene back inside the box (mutation can overshoot)."""
        return tuple(min(self.hi, max(self.lo, float(g))) for g in genome)

    def random(self, rng: random.Random) -> Genome:
        """One uniformly drawn genome."""
        return tuple(rng.uniform(self.lo, self.hi) for _ in range(self.dim))

    def blend(self, rng: random.Random, a: Genome, b: Genome,
              alpha: float = 0.35) -> Genome:
        """BLX-alpha crossover: a child anywhere near the segment [a, b], which
        keeps exploring outside the parents instead of interpolating only."""
        out: List[float] = []
        for x, y in zip(a, b):
            lo, hi = min(x, y), max(x, y)
            pad = alpha * (hi - lo)
            out.append(rng.uniform(lo - pad, hi + pad))
        return self.clamp(out)

    def mutate(self, rng: random.Random, genome: Genome, sigma: float) -> Genome:
        """Adaptive Gaussian mutation, one shared step for the whole genome."""
        return self.clamp(tuple(g + rng.gauss(0.0, sigma) for g in genome))

    @staticmethod
    def spread(population: Sequence[Candidate]) -> float:
        """Diversity: the mean per-gene standard deviation of the population."""
        if len(population) < 2:
            return 0.0
        width = len(population[0].genome)
        per_gene = [statistics.pstdev([c.genome[i] for c in population])
                    for i in range(width)]
        return statistics.fmean(per_gene) if per_gene else 0.0


class _BudgetedSearch:
    """Shared bookkeeping: evaluation counter, wall-clock deadline, best seen."""

    def __init__(self, space: Space, evaluate: Callable[[Genome], Candidate],
                 rng: random.Random, budget: int,
                 time_budget: Optional[float] = None,
                 clock: Callable[[], float] = time.monotonic) -> None:
        if budget <= 0:
            raise ValueError("the budget must allow at least one evaluation")
        self.space = space
        self._evaluate = evaluate
        self.rng = rng
        self.budget = budget
        self.deadline = None if time_budget is None else clock() + float(time_budget)
        self.clock = clock
        self.evaluations = 0
        self.best: Optional[Candidate] = None

    def evaluate(self, genome: Genome) -> Optional[Candidate]:
        """One paid evaluation, or None when the budget is already spent."""
        if self.evaluations >= self.budget:
            return None
        candidate = self._evaluate(self.space.clamp(genome))
        self.evaluations += 1
        if self.best is None or candidate.outranks(self.best):
            self.best = candidate
        return candidate

    def expired(self) -> bool:
        """True when the wall-clock budget is over."""
        return self.deadline is not None and self.clock() >= self.deadline

    def room(self, count: int) -> bool:
        """True when `count` more evaluations fit in the budget."""
        return self.evaluations + count <= self.budget and not self.expired()


class EvoSearch(_BudgetedSearch):
    """Evolutionary loop: tournament selection, BLX-alpha crossover, adaptive
    Gaussian mutation (1/5th success rule), elitism."""

    def __init__(self, space: Space, evaluate: Callable[[Genome], Candidate],
                 rng: random.Random, budget: int = 400,
                 time_budget: Optional[float] = None, population: int = 24,
                 elite: int = 4, tournament: int = 3, sigma: float = 0.20,
                 sigma_min: float = 0.02, sigma_max: float = 0.60,
                 grow: float = 1.2, shrink: float = 0.85, stall: int = 10,
                 seeds: Sequence[Genome] = (),
                 clock: Callable[[], float] = time.monotonic) -> None:
        super().__init__(space, evaluate, rng, budget, time_budget, clock)
        self.population = max(4, population)
        self.elite = max(1, min(elite, self.population // 2))
        self.tournament = max(2, min(tournament, self.population))
        self.sigma = sigma
        self.sigma_min, self.sigma_max = sigma_min, sigma_max
        self.grow, self.shrink = grow, shrink
        self.stall_limit = max(2, stall)
        self.seeds = list(seeds)

    def _tournament(self, pop: Sequence[Candidate]) -> Candidate:
        """Pick the best of `tournament` random members."""
        drawn = [pop[self.rng.randrange(len(pop))] for _ in range(self.tournament)]
        best = drawn[0]
        for candidate in drawn[1:]:
            if candidate.outranks(best):
                best = candidate
        return best

    def _init_population(self) -> List[Candidate]:
        """Warm start on the given archetypes, then random genomes."""
        pop: List[Candidate] = []
        for genome in self.seeds:
            candidate = self.evaluate(genome)
            if candidate is None:
                break
            pop.append(candidate)
        while len(pop) < self.population:
            candidate = self.evaluate(self.space.random(self.rng))
            if candidate is None:
                break
            pop.append(candidate)
        return pop

    def _generation(self, pop: List[Candidate]) -> List[Candidate]:
        """One generation: the elite survives, the rest is bred and evaluated."""
        pop.sort(key=lambda c: (c.error, not c.counter_example, -c.fitness))
        children = list(pop[:self.elite])
        while len(children) < self.population and self.room(1):
            a = self._tournament(pop).genome
            b = self._tournament(pop).genome
            child = self.space.blend(self.rng, a, b)
            if self.rng.random() < 0.5:
                child = self.space.mutate(self.rng, child, self.sigma)
            else:
                child = self.space.mutate(self.rng, a, self.sigma)
            evaluated = self.evaluate(child)
            if evaluated is None:
                break
            children.append(evaluated)
        return children

    def run(self) -> SearchResult:
        """Run the evolution until the budget is spent or the search stalls."""
        started = self.clock()
        pop = self._init_population()
        history: List[Dict[str, Any]] = []
        generations = 0
        stalled = False
        previous = self.best.fitness if self.best else -math.inf
        quiet = 0
        while pop and self.room(self.population - self.elite):
            pop = self._generation(pop)
            generations += 1
            best = self.best.fitness if self.best else -math.inf
            if best > previous + 1e-12:
                previous, quiet = best, 0
                self.sigma = min(self.sigma_max, self.sigma * self.grow)
            else:
                quiet += 1
                self.sigma = max(self.sigma_min, self.sigma * self.shrink)
            history.append({"gen": generations, "evals": self.evaluations,
                            "best": best,
                            "mean": statistics.fmean([c.fitness for c in pop]),
                            "diversity": self.space.spread(pop),
                            "sigma": round(self.sigma, 5)})
            if quiet >= self.stall_limit:
                stalled = True
                break
        return SearchResult(best=self.best, evaluations=self.evaluations,
                            generations=generations, seconds=self.clock() - started,
                            algorithm="evo", stalled=stalled, history=history)


class AnnealSearch(_BudgetedSearch):
    """Simulated annealing with geometric cooling: the fallback that polishes a
    single point when the population has converged."""

    def __init__(self, space: Space, evaluate: Callable[[Genome], Candidate],
                 rng: random.Random, budget: int = 200,
                 time_budget: Optional[float] = None, t_start: float = 1.0,
                 t_end: float = 0.02, step: float = 0.18,
                 clock: Callable[[], float] = time.monotonic) -> None:
        super().__init__(space, evaluate, rng, budget, time_budget, clock)
        self.t_start, self.t_end, self.step = t_start, t_end, step

    def _accept(self, current: Candidate, proposed: Candidate,
                temperature: float) -> bool:
        """Accept an improvement, or a regression with the Metropolis rule. The
        temperature is scaled by the current fitness so that the huge weight of
        a counter-example does not freeze the chain."""
        if proposed.outranks(current):
            return True
        delta = proposed.fitness - current.fitness
        scale = max(1.0, abs(current.fitness)) * temperature
        return self.rng.random() < math.exp(delta / scale)

    def run(self, start: Optional[Genome] = None) -> SearchResult:
        """Anneal from `start` (or a random genome) until the budget is spent."""
        began = self.clock()
        current = self.evaluate(start if start is not None
                                else self.space.random(self.rng))
        if current is None:
            return SearchResult(best=self.best, evaluations=self.evaluations,
                                generations=0, seconds=self.clock() - began,
                                algorithm="anneal")
        history: List[Dict[str, Any]] = []
        steps = max(1, self.budget - self.evaluations)
        cooling = (self.t_end / self.t_start) ** (1.0 / steps)
        temperature = self.t_start
        moves = 0
        while self.room(1):
            temperature *= cooling
            proposed = self.evaluate(
                self.space.mutate(self.rng, current.genome, self.step))
            if proposed is None:
                break
            moves += 1
            if self._accept(current, proposed, temperature):
                current = proposed
            if moves % 10 == 0:
                history.append({"gen": moves, "evals": self.evaluations,
                                "best": self.best.fitness if self.best else 0.0,
                                "current": current.fitness,
                                "temperature": round(temperature, 6)})
        return SearchResult(best=self.best, evaluations=self.evaluations,
                            generations=moves, seconds=self.clock() - began,
                            algorithm="anneal", history=history)


def search(space: Space, evaluate: Callable[[Genome], Candidate],
           rng: random.Random, budget: int = 400,
           time_budget: Optional[float] = None, algorithm: str = "auto",
           seeds: Sequence[Genome] = (), evo_share: float = 0.75,
           **kwargs: Any) -> SearchResult:
    """One search under a global budget.

    `evo` and `anneal` run the matching algorithm alone; `auto` runs the
    evolution on `evo_share` of the budget and, when it stalls, spends the rest
    in the annealing fallback seeded with the best genome found so far.
    """
    if algorithm == "anneal":
        return AnnealSearch(space, evaluate, rng, budget, time_budget,
                            **kwargs).run()
    if algorithm not in ("evo", "auto"):
        raise ValueError("algorithm must be auto, evo or anneal")
    evo_budget = budget if algorithm == "evo" else max(8, int(budget * evo_share))
    evo = EvoSearch(space, evaluate, rng, evo_budget, time_budget, seeds=seeds,
                    **kwargs)
    first = evo.run()
    if algorithm == "evo" or not first.stalled or evo.evaluations >= budget:
        return first
    rest = budget - evo.evaluations
    if rest < 4 or evo.expired():
        return first
    polish = AnnealSearch(space, evaluate, rng, rest, None,
                          **_anneal_kwargs(kwargs))
    second = polish.run(first.best.genome if first.best else None)
    return SearchResult(best=second.best, evaluations=evo.evaluations + second.evaluations,
                        generations=first.generations + second.generations,
                        seconds=first.seconds + second.seconds,
                        algorithm="evo+anneal", stalled=second.stalled,
                        history=first.history + second.history)


def _anneal_kwargs(kwargs: Dict[str, Any]) -> Dict[str, Any]:
    """Keep only the annealing knobs from a shared keyword bag."""
    return {k: kwargs[k] for k in ("t_start", "t_end", "step") if k in kwargs}
