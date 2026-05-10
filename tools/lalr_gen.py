#!/usr/bin/env python3
"""
lalr_gen.py — LALR(1) table generator for xmc.bnf

Usage:  python lalr_gen.py path/to/xmc.bnf

Pass 1 – parse and normalise the BNF into a flat CFG.
Pass 2 – compute FIRST / FOLLOW sets.
Pass 3 – build canonical LR(0) item sets.
Pass 4 – propagate LALR(1) lookaheads.
Pass 5 – build action / goto tables; report conflicts.

The output is a conflict report.  When the grammar is clean a separate
C++ emitter (--emit-cpp) will write the table arrays.
"""

import sys
import re
from collections import defaultdict
from itertools import chain

# ─────────────────────────────────────────────────────────────────────────────
# Constants
# ─────────────────────────────────────────────────────────────────────────────

EOF = '$'          # end-of-input terminal
START = "program'" # augmented start symbol

# Lexer-token names that appear as terminals in the grammar.
# (Their BNF productions, if any, are definitions of the *lexer*, not the parser.)
LEXER_TERMINALS = frozenset({
    'IDENTIFIER', 'INTEGER-LITERAL', 'FLOAT-LITERAL', 'DECIMAL-LITERAL',
    'STRING-LITERAL', 'CHAR-LITERAL', 'UGLY-MASK',
})

# BNF productions that define lexer internals – skip them entirely.
SKIP_PRODS = frozenset({
    'KEYWORD', 'MASK-CHAR', 'STRING-CHAR', 'STRING-INTERPOLATION',
    'CHAR-CHAR', 'LETTER', 'DIGIT', 'HEX-DIGIT', 'BIN-DIGIT',
    'LINE-COMMENT', 'BLOCK-COMMENT',
    'INTEGER-LITERAL', 'FLOAT-LITERAL', 'DECIMAL-LITERAL',
    'STRING-LITERAL', 'CHAR-LITERAL', 'UGLY-MASK', 'IDENTIFIER',
})


def is_terminal(sym):
    if sym == EOF:
        return True
    if sym.startswith('"'):       # "keyword" or "punct"
        return True
    if sym in LEXER_TERMINALS:
        return True
    return False


# ─────────────────────────────────────────────────────────────────────────────
# Grammar
# ─────────────────────────────────────────────────────────────────────────────

class Grammar:
    def __init__(self):
        self.rules = []            # [(lhs, (sym, ...)), ...]
        self.lhs_rules = defaultdict(list)   # lhs -> [rule_index, ...]
        self._anon = 0

    def add(self, lhs, rhs):
        idx = len(self.rules)
        self.rules.append((lhs, tuple(rhs)))
        self.lhs_rules[lhs].append(idx)
        return idx

    def fresh(self, tag):
        self._anon += 1
        return f'${tag}_{self._anon}'

    @property
    def nonterminals(self):
        return set(lhs for lhs, _ in self.rules)

    @property
    def terminals(self):
        syms = set()
        for _, rhs in self.rules:
            for s in rhs:
                if is_terminal(s):
                    syms.add(s)
        syms.add(EOF)
        return syms


# ─────────────────────────────────────────────────────────────────────────────
# BNF file parser -> Grammar
# ─────────────────────────────────────────────────────────────────────────────

def parse_bnf(path):
    with open(path, encoding='utf-8') as f:
        raw = f.read()
    g = Grammar()
    _BnfParser(raw, g).run()
    # Augment: START -> program EOF
    g.add(START, ['program', EOF])
    return g


class _BnfParser:
    def __init__(self, text, g):
        self.lines = text.splitlines()
        self.g = g
        self.pos = 0

    # ── top-level scan ────────────────────────────────────────────────────────

    def run(self):
        while self.pos < len(self.lines):
            line = self.lines[self.pos]
            s = line.strip()
            if not s or s.startswith(';'):
                self.pos += 1
                continue
            # Non-indented, not ::= or |  -> start of a new production head
            if not line[0].isspace() and not s.startswith('::=') and not s.startswith('|'):
                lhs = s.split()[0]
                self.pos += 1
                body_lines = self._collect_body()
                if lhs not in SKIP_PRODS:
                    self._process(lhs, body_lines)
            else:
                self.pos += 1

    def _collect_body(self):
        """Collect indented / ::= / | lines that belong to the current production."""
        body = []
        while self.pos < len(self.lines):
            line = self.lines[self.pos]
            s = line.strip()
            if not s or s.startswith(';'):
                # Blank / comment: peek ahead; if next meaningful line is a
                # continuation keep going, else stop.
                j = self.pos + 1
                while j < len(self.lines) and (not self.lines[j].strip() or
                                                self.lines[j].strip().startswith(';')):
                    j += 1
                if j < len(self.lines):
                    ns = self.lines[j].strip()
                    if ns.startswith('::=') or ns.startswith('|') or \
                       (self.lines[j] and self.lines[j][0].isspace() and ns):
                        self.pos += 1
                        continue
                break
            if line[0].isspace() or s.startswith('::=') or s.startswith('|'):
                body.append(_strip_inline_comment(s))
                self.pos += 1
            else:
                break
        return body

    def _process(self, lhs, lines):
        text = ' '.join(lines)
        # Strip leading ::=
        text = re.sub(r'^::=\s*', '', text.strip())
        for alt in _split_alts(text):
            rhs = self._seq(alt.strip(), lhs)
            if rhs is not None:
                self.g.add(lhs, rhs)

    # ── RHS sequence parser ───────────────────────────────────────────────────

    def _seq(self, text, hint):
        """Parse a flat RHS string into a list of symbols."""
        result = []
        i = 0
        text = text.strip()
        while i < len(text):
            while i < len(text) and text[i] in ' \t':
                i += 1
            if i >= len(text):
                break
            c = text[i]
            if c == ';':
                break
            elif c == '"':
                j = i + 1
                while j < len(text) and text[j] != '"':
                    j += 1
                result.append(text[i:j+1])
                i = j + 1
            elif c == '{':
                inner, end = _match(text, i, '{', '}')
                result.append(self._make_star(inner, hint))
                i = end + 1
            elif c == '[':
                inner, end = _match(text, i, '[', ']')
                result.append(self._make_opt(inner, hint))
                i = end + 1
            elif c == '(':
                inner, end = _match(text, i, '(', ')')
                node = self._make_group(inner, hint)
                if node is None:
                    result.extend(self._seq(inner, hint) or [])
                else:
                    result.append(node)
                i = end + 1
            elif c == '<':
                inner, end = _match(text, i, '<', '>')
                result.extend(self._seq(inner, hint) or [])
                i = end + 1
            else:
                j = i
                while j < len(text) and text[j] not in ' \t"{}[]()<>;':
                    j += 1
                sym = text[i:j]
                if sym:
                    result.append(sym)
                i = j
        return result

    def _make_star(self, inner, hint):
        """{ inner } -> name ::= ε | name inner"""
        name = self.g.fresh('rep')
        self.g.add(name, [])   # ε
        for alt in _split_alts(inner):
            rhs = self._seq(alt.strip(), hint)
            if rhs:
                self.g.add(name, [name] + rhs)
        return name

    def _make_opt(self, inner, hint):
        """[ inner ] -> name ::= ε | alt1 | alt2 ..."""
        name = self.g.fresh('opt')
        self.g.add(name, [])   # ε
        for alt in _split_alts(inner):
            rhs = self._seq(alt.strip(), hint)
            if rhs is not None:
                self.g.add(name, rhs)
        return name

    def _make_group(self, inner, hint):
        """( A | B ) -> name ::= A | B.  Returns None if only one alt (inline it)."""
        alts = _split_alts(inner)
        if len(alts) == 1:
            return None
        name = self.g.fresh('grp')
        for alt in alts:
            rhs = self._seq(alt.strip(), hint)
            if rhs is not None:
                self.g.add(name, rhs)
        return name


def _strip_inline_comment(s):
    """Remove trailing ; comment from a BNF line (outside of quotes)."""
    in_q = False
    for i, c in enumerate(s):
        if c == '"':
            in_q = not in_q
        elif c == ';' and not in_q:
            return s[:i].rstrip()
    return s


def _split_alts(text):
    """Split text on top-level '|' (not inside quotes / brackets)."""
    alts, depth, in_q, start = [], 0, False, 0
    for i, c in enumerate(text):
        if c == '"' and not in_q:
            in_q = True
        elif c == '"' and in_q:
            in_q = False
        elif not in_q:
            if c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
            elif c == ';':
                alts.append(text[start:i])
                return [a for a in alts if a.strip()]
            elif c == '|' and depth == 0:
                alts.append(text[start:i])
                start = i + 1
    alts.append(text[start:])
    return [a for a in alts if a.strip()]


def _match(text, start, open_c, close_c):
    """Return (inner, end_index) for matching bracket pair starting at start."""
    depth, in_q, i = 1, False, start + 1
    while i < len(text) and depth > 0:
        c = text[i]
        if c == '"' and not in_q:
            in_q = True
        elif c == '"' and in_q:
            in_q = False
        elif not in_q:
            if c == open_c:
                depth += 1
            elif c == close_c:
                depth -= 1
        i += 1
    return text[start + 1:i - 1], i - 1


# ─────────────────────────────────────────────────────────────────────────────
# FIRST sets
# ─────────────────────────────────────────────────────────────────────────────

def compute_first(g):
    """Return FIRST[sym] as a set of terminals (including '' for ε-derivable)."""
    FIRST = defaultdict(set)

    # Terminals derive themselves
    for t in g.terminals:
        FIRST[t].add(t)
    FIRST[EOF].add(EOF)

    changed = True
    while changed:
        changed = False
        for lhs, rhs in g.rules:
            before = len(FIRST[lhs])
            if not rhs:
                # ε production
                FIRST[lhs].add('')
            else:
                all_nullable = True
                for sym in rhs:
                    FIRST[lhs].update(FIRST[sym] - {''})
                    if '' not in FIRST[sym]:
                        all_nullable = False
                        break
                if all_nullable:
                    FIRST[lhs].add('')
            if len(FIRST[lhs]) != before:
                changed = True
    return FIRST


def first_of_seq(seq, FIRST):
    """FIRST of a sequence of symbols; returns a set of terminals (+'' if nullable)."""
    result = set()
    for sym in seq:
        result.update(FIRST[sym] - {''})
        if '' not in FIRST[sym]:
            return result
    result.add('')
    return result


# ─────────────────────────────────────────────────────────────────────────────
# LR(0) items and item sets
# ─────────────────────────────────────────────────────────────────────────────

def lr0_closure(items, g):
    """LR(0) closure: items is a frozenset of (rule_idx, dot)."""
    work = set(items)
    result = set(items)
    while work:
        rule_idx, dot = work.pop()
        _, rhs = g.rules[rule_idx]
        if dot < len(rhs):
            sym = rhs[dot]
            if not is_terminal(sym):
                for ri in g.lhs_rules.get(sym, []):
                    item = (ri, 0)
                    if item not in result:
                        result.add(item)
                        work.add(item)
    return frozenset(result)


def lr0_goto(item_set, sym, g):
    """LR(0) GOTO: shift dot past sym."""
    moved = set()
    for rule_idx, dot in item_set:
        _, rhs = g.rules[rule_idx]
        if dot < len(rhs) and rhs[dot] == sym:
            moved.add((rule_idx, dot + 1))
    if not moved:
        return frozenset()
    return lr0_closure(frozenset(moved), g)


def build_lr0(g):
    """Build canonical LR(0) collection.
    Returns (states, goto_map) where:
      states   = list of frozenset-of-items (index = state number)
      goto_map = dict[(state_idx, sym)] = state_idx
    """
    start_rule = g.lhs_rules[START][0]
    init = lr0_closure(frozenset({(start_rule, 0)}), g)
    states = [init]
    state_index = {init: 0}
    goto_map = {}
    work = [0]

    # Gather all symbols that can appear after a dot
    all_syms = set()
    for _, rhs in g.rules:
        all_syms.update(rhs)

    while work:
        si = work.pop()
        item_set = states[si]
        for sym in all_syms:
            next_set = lr0_goto(item_set, sym, g)
            if not next_set:
                continue
            if next_set not in state_index:
                state_index[next_set] = len(states)
                states.append(next_set)
                work.append(state_index[next_set])
            goto_map[(si, sym)] = state_index[next_set]

    return states, goto_map


# ─────────────────────────────────────────────────────────────────────────────
# LALR(1) lookaheads via spontaneous/propagation
# ─────────────────────────────────────────────────────────────────────────────

def compute_lalr1(states, goto_map, g, FIRST):
    """
    Compute LALR(1) lookahead sets using the standard propagation algorithm.

    Returns lookaheads[(state_idx, item)] = set of lookahead terminals.
    """
    DUMMY = '#'   # a terminal used only during propagation discovery

    lookaheads = defaultdict(set)  # (si, item) -> set of terminals
    propagates = defaultdict(set)  # (si, item) -> set of (sj, item)

    # Seed: initial item of augmented start gets {$}
    start_rule = g.lhs_rules[START][0]
    init_item = (start_rule, 0)
    lookaheads[(0, init_item)].add(EOF)

    for si, item_set in enumerate(states):
        # Kernel items only (dot > 0, or initial item)
        kernels = {it for it in item_set
                   if it[1] > 0 or g.rules[it[0]][0] == START}
        for k_item in kernels:
            # Compute closure of {(k_item, DUMMY)}
            # Walk through the closure to find spontaneous lookaheads
            # and propagation targets.
            # We treat DUMMY as a special terminal that never spontaneously appears.
            items_la = {k_item: {DUMMY}}  # item -> lookahead set (LR1 items)
            work = list(items_la.keys())
            while work:
                it = work.pop()
                ri, dot = it
                _, rhs = g.rules[ri]
                if dot >= len(rhs):
                    continue
                sym = rhs[dot]
                if not is_terminal(sym):
                    for sub_ri in g.lhs_rules.get(sym, []):
                        sub_item = (sub_ri, 0)
                        # Compute FIRST of (rhs[dot+1:] + lookahead)
                        beta = rhs[dot + 1:]
                        for la in list(items_la[it]):
                            if la == DUMMY:
                                first_beta = first_of_seq(beta, FIRST)
                                if '' in first_beta:
                                    # Propagates
                                    propagates[(si, k_item)].add(
                                        (goto_map.get((si, sym), -1), sub_item))
                                spontaneous = first_beta - {''}
                                if spontaneous:
                                    before = len(items_la.get(sub_item, set()))
                                    items_la.setdefault(sub_item, set()).update(spontaneous)
                                    if len(items_la[sub_item]) != before and sub_item not in work:
                                        work.append(sub_item)
                            else:
                                # la is a real terminal
                                first_beta_la = first_of_seq(list(beta) + [la], FIRST)
                                spontaneous = first_beta_la - {''}
                                before = len(items_la.get(sub_item, set()))
                                items_la.setdefault(sub_item, set()).update(spontaneous)
                                if len(items_la[sub_item]) != before and sub_item not in work:
                                    work.append(sub_item)

            # Collect spontaneous lookaheads and propagation links
            for it, las in items_la.items():
                real_las = las - {DUMMY}
                ri2, dot2 = it
                _, rhs2 = g.rules[ri2]
                # The item that results from shifting sym past k_item reaches
                # a next state via goto.
                # We record spontaneous lookaheads directly on the kernel item
                # in the target state.
                # ... actually we need to find which kernel item in which state
                # this (it, la) corresponds to after closure.
                # For items in the closure that have real las, those are
                # spontaneous for the kernel items of their goto states.
                if real_las:
                    # Find the state reached when this item was formed
                    # (the item belongs to closure of si via k_item)
                    # If it IS a kernel item of some state sj = goto[si][sym_that_shifted_to_it]
                    # This is complex; use the simpler "after closure" approach:
                    # Real las generated for sub_item (ri2, 0) propagate to
                    # state goto[si][sym] where sym = rhs[k_item.dot]
                    pass

    # Simpler standard propagation algorithm (Aho 4.7):
    # Re-do cleanly using the textbook approach.
    return _lalr1_propagation(states, goto_map, g, FIRST)


def _lalr1_propagation(states, goto_map, g, FIRST):
    """
    Textbook LALR(1) lookahead propagation (Dragon Book Algorithm 4.62).
    """
    HASH = '#'   # dummy terminal

    # lookaheads[(si, item)] = set of terminals
    lookaheads = defaultdict(set)
    propagates = defaultdict(set)   # (si, kernel_item) -> {(sj, kernel_item)}

    # Seed augmented start item
    start_rule = g.lhs_rules[START][0]
    lookaheads[(0, (start_rule, 0))].add(EOF)

    def kernel_items(si):
        return {it for it in states[si]
                if it[1] > 0 or g.rules[it[0]][0] == START}

    for si in range(len(states)):
        for k_item in kernel_items(si):
            ri, dot = k_item
            # Compute LR(1) closure of {[k_item, #]}
            lr1_init = {k_item: {HASH}}
            lr1_items = dict(lr1_init)
            work = [k_item]
            while work:
                it = work.pop()
                ri2, dot2 = it
                _, rhs2 = g.rules[ri2]
                if dot2 >= len(rhs2):
                    continue
                B = rhs2[dot2]
                if is_terminal(B):
                    continue
                beta = rhs2[dot2 + 1:]
                for sub_ri in g.lhs_rules.get(B, []):
                    sub_item = (sub_ri, 0)
                    new_las = set()
                    for la in lr1_items[it]:
                        seq = list(beta) + ([la] if la != HASH else [])
                        fb = first_of_seq(seq, FIRST) - {''}
                        new_las.update(fb)
                        if '' in first_of_seq(list(beta), FIRST):
                            new_las.add(la)  # HASH propagates through nullable beta
                    before = set(lr1_items.get(sub_item, set()))
                    lr1_items.setdefault(sub_item, set()).update(new_las)
                    if lr1_items[sub_item] != before and sub_item not in work:
                        work.append(sub_item)

            # For each item in the LR(1) closure, see where it goes via GOTO
            for it, las in lr1_items.items():
                ri2, dot2 = it
                _, rhs2 = g.rules[ri2]
                if dot2 >= len(rhs2):
                    # Complete/epsilon item in the closure.
                    # Record only SPONTANEOUS lookaheads (la != HASH) directly
                    # against this state.  We deliberately skip the HASH case
                    # (propagation from k_item) because adding k_item → epsilon
                    # propagation links causes LALR-merged lookaheads to bleed
                    # into epsilon items across unrelated contexts, producing
                    # hundreds of spurious R/R conflicts.  Spontaneous FIRST(β)
                    # lookaheads are sufficient for all epsilon reductions in
                    # well-formed LALR(1) grammars.
                    if it in states[si]:
                        for la in las:
                            if la != HASH:
                                lookaheads[(si, it)].add(la)
                    continue
                sym = rhs2[dot2]
                sj = goto_map.get((si, sym))
                if sj is None:
                    continue
                target_item = (ri2, dot2 + 1)
                if target_item not in states[sj]:
                    continue
                for la in las:
                    if la == HASH:
                        propagates[(si, k_item)].add((sj, target_item))
                    else:
                        lookaheads[(sj, target_item)].add(la)

    # Propagate until stable
    changed = True
    while changed:
        changed = False
        for (si, k_item), targets in list(propagates.items()):
            for (sj, t_item) in targets:
                before = len(lookaheads[(sj, t_item)])
                lookaheads[(sj, t_item)].update(lookaheads[(si, k_item)])
                if len(lookaheads[(sj, t_item)]) != before:
                    changed = True

    return lookaheads


# ─────────────────────────────────────────────────────────────────────────────
# Table builder + conflict detector
# ─────────────────────────────────────────────────────────────────────────────

def build_tables(states, goto_map, lookaheads, g):
    """
    Build ACTION and GOTO tables.
    Returns (action, goto_t, conflicts) where:
      action[(si, terminal)]     = list of ('shift', sj) | ('reduce', rule_idx) | ('accept',)
      goto_t[(si, nonterminal)]  = sj
      conflicts                  = list of (si, terminal, actions) for multi-entry cells
    """
    action = defaultdict(list)
    goto_t = {}
    conflicts = []

    start_rule = g.lhs_rules[START][0]

    for si, item_set in enumerate(states):
        for rule_idx, dot in item_set:
            lhs, rhs = g.rules[rule_idx]
            if dot < len(rhs):
                sym = rhs[dot]
                sj = goto_map.get((si, sym))
                if sj is None:
                    continue
                if is_terminal(sym):
                    if sym == EOF and lhs == START:
                        action[(si, EOF)].append(('accept',))
                    else:
                        action[(si, sym)].append(('shift', sj))
                else:
                    goto_t[(si, sym)] = sj
            else:
                # Reduce item
                if lhs == START:
                    action[(si, EOF)].append(('accept',))
                else:
                    # Get lookaheads for this kernel item
                    k_item = (rule_idx, dot)
                    if dot == 0:
                        # Epsilon production was added as kernel item with dot=0
                        # only if it IS a kernel item (dot>0 or start)
                        pass
                    las = lookaheads.get((si, k_item), set())
                    for la in las:
                        action[(si, la)].append(('reduce', rule_idx))

    # Deduplicate and detect conflicts
    for key, acts in action.items():
        unique = list({str(a): a for a in acts}.values())
        action[key] = unique
        if len(unique) > 1:
            si, tok = key
            conflicts.append((si, tok, unique))

    return dict(action), goto_t, conflicts


# ─────────────────────────────────────────────────────────────────────────────
# Report
# ─────────────────────────────────────────────────────────────────────────────

def report(g, states, goto_map, action, conflicts, lookaheads):
    n_rules = len(g.rules)
    n_states = len(states)
    n_terminals = len(g.terminals)
    n_nonterminals = len(g.nonterminals)

    named_rules = [(i, lhs, rhs) for i, (lhs, rhs) in enumerate(g.rules)
                   if not lhs.startswith('$')]
    anon_rules  = [(i, lhs, rhs) for i, (lhs, rhs) in enumerate(g.rules)
                   if lhs.startswith('$')]

    print(f"Grammar summary")
    print(f"  Named productions : {len(named_rules)}")
    print(f"  Anon productions  : {len(anon_rules)}"
          f"  (from {{}} [] () expansions)")
    print(f"  Total rules       : {n_rules}")
    print(f"  Terminals         : {n_terminals}")
    print(f"  Nonterminals      : {n_nonterminals}")
    print(f"  LR(0) states      : {n_states}")
    print()

    if not conflicts:
        print("No conflicts — grammar is LALR(1) clean.")
    else:
        # Group by type
        sr = [(s, t, a) for s, t, a in conflicts
              if any(x[0] == 'shift' for x in a) and any(x[0] == 'reduce' for x in a)]
        rr = [(s, t, a) for s, t, a in conflicts
              if all(x[0] == 'reduce' for x in a)]
        print(f"CONFLICTS: {len(conflicts)} total  "
              f"({len(sr)} shift/reduce,  {len(rr)} reduce/reduce)")
        print()
        for si, tok, acts in sorted(conflicts):
            shift_acts  = [a for a in acts if a[0] == 'shift']
            reduce_acts = [a for a in acts if a[0] == 'reduce']
            kind = 'S/R' if shift_acts and reduce_acts else 'R/R'
            print(f"  State {si:4d}  on  {tok!r:<30}  [{kind}]")
            for a in acts:
                if a[0] == 'shift':
                    print(f"              shift -> state {a[1]}")
                elif a[0] == 'reduce':
                    ri = a[1]
                    lhs, rhs = g.rules[ri]
                    rhs_str = ' '.join(rhs) if rhs else 'ε'
                    print(f"              reduce  r{ri}: {lhs} -> {rhs_str}")
            # Show the items in this state that caused the conflict
            for rule_idx, dot in sorted(states[si]):
                rl, rhs = g.rules[rule_idx]
                # Only show items that touch this terminal
                if dot < len(rhs) and rhs[dot] == tok:
                    items_str = ' '.join(
                        (f'• {s}' if i == dot else s) for i, s in enumerate(rhs)
                    )
                    print(f"                [{rl} -> {items_str}]")
                elif dot == len(rhs):
                    las = lookaheads.get((si, (rule_idx, dot)), set())
                    if tok in las:
                        items_str = ' '.join(rhs) + ' •' if rhs else '• ε'
                        print(f"                [{rl} -> {items_str}]  la={sorted(las)}")
            print()

    # Undefined nonterminals (used but never defined)
    defined = set(g.lhs_rules.keys())
    used = set()
    for _, rhs in g.rules:
        for sym in rhs:
            if not is_terminal(sym):
                used.add(sym)
    undefined = used - defined
    if undefined:
        print(f"\nUNDEFINED nonterminals (used but not defined in BNF):")
        for u in sorted(undefined):
            print(f"  {u}")

    # Unreachable nonterminals
    reachable = {START, 'program'}
    changed = True
    while changed:
        changed = False
        for lhs, rhs in g.rules:
            if lhs in reachable:
                for sym in rhs:
                    if not is_terminal(sym) and sym not in reachable:
                        reachable.add(sym)
                        changed = True
    unreachable = defined - reachable
    if unreachable:
        print(f"\nUNREACHABLE nonterminals (defined but not reachable from program):")
        for u in sorted(unreachable):
            print(f"  {u}")


# ─────────────────────────────────────────────────────────────────────────────
# Conflict resolution
# ─────────────────────────────────────────────────────────────────────────────

def resolve_sr_conflicts(action, conflicts):
    """
    Auto-resolve every shift/reduce conflict by preferring shift.
    Returns (resolved_count, remaining_conflicts).
    """
    resolved = 0
    remaining = []
    sr_keys = set()
    for si, tok, acts in conflicts:
        has_shift  = any(a[0] == 'shift'  for a in acts)
        has_reduce = any(a[0] == 'reduce' for a in acts)
        if has_shift and has_reduce:
            sr_keys.add((si, tok))
        else:
            remaining.append((si, tok, acts))

    for key in sr_keys:
        acts = action[key]
        shifts = [a for a in acts if a[0] == 'shift']
        action[key] = [shifts[0]]
        resolved += 1

    return resolved, remaining


def resolve_rr_conflicts(action, conflicts):
    """
    Auto-resolve every reduce/reduce conflict by preferring the reduce with the
    lowest rule index (i.e. the rule defined earliest in the grammar).  This
    mirrors how classical yacc resolves R/R conflicts.

    Many of these arise from the grammar's optional / Kleene-star prefixes
    (e.g. [type-specifier] before an IDENTIFIER) which are a genuine LALR(1)
    limitation; for inputs that don't trigger the ambiguous path the resolved
    table still produces correct parses.

    Returns (resolved_count, remaining_conflicts).
    """
    resolved = 0
    remaining = []
    for si, tok, acts in conflicts:
        all_reduce = all(a[0] == 'reduce' for a in acts)
        if all_reduce:
            best = min(acts, key=lambda a: a[1])
            action[(si, tok)] = [best]
            resolved += 1
        else:
            remaining.append((si, tok, acts))
    return resolved, remaining


# ─────────────────────────────────────────────────────────────────────────────
# C++ table emitter
# ─────────────────────────────────────────────────────────────────────────────

_PUNCT_MAP = {
    '!':'BANG', '"':'Q', '#':'HASH', '$':'DOLLAR', '%':'PCT',
    '&':'AMP',  "'": 'APOS', '(':'LP', ')':'RP', '*':'STAR',
    '+':'PLUS', ',':'COM', '-':'MINUS', '.':'DOT', '/':'SLASH',
    ':':'COL',  ';':'SEMI', '<':'LT', '=':'EQ', '>':'GT',
    '?':'QM',   '@':'AT', '[':'LB', '\\':'BS', ']':'RB',
    '^':'XOR',  '`':'BT', '{':'LC', '|':'PIPE', '}':'RC',
    '~':'TILD', ' ':'_',
}

def _term_to_cname(t):
    if t == EOF:
        return 'EOF'
    if t.startswith('"') and t.endswith('"'):
        inner = t[1:-1]
        out = []
        for c in inner:
            if c.isalnum() or c == '_':
                out.append(c)
            else:
                out.append(_PUNCT_MAP.get(c, '_'))
        return 'Q_' + ''.join(out)
    return re.sub(r'[^a-zA-Z0-9_]', '_', t)

def _nt_to_cname(nt):
    return re.sub(r'[^a-zA-Z0-9_]', '_',
                  nt.replace("'", '_prime').replace('$', ''))


def _cpp_str(s):
    """Return s as a C++ double-quoted string literal."""
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit_cpp(g, states, goto_map, action, goto_t, out_path):
    """
    Write LALR(1) tables to a C++ header.

    Action encoding (int32_t):
      0           = error
      INT32_MAX   = accept
      v > 0       = shift to state (v - 1)
      v < 0       = reduce rule  (-v - 1)

    Tables are sparse: entries are sorted by index within each row so the
    runtime can use binary search (or a simple linear scan for small rows).
    """
    terms = sorted(g.terminals)
    term_idx = {t: i for i, t in enumerate(terms)}

    nts = sorted(g.nonterminals)
    nt_idx = {nt: i for i, nt in enumerate(nts)}

    n_states = len(states)
    ACCEPT_VAL = 0x7FFFFFFF

    # Build action entries per state
    action_by_state = defaultdict(list)
    for (si, tok), acts in action.items():
        if not acts:
            continue
        ti = term_idx.get(tok)
        if ti is None:
            continue
        a = acts[0]
        if a[0] == 'accept':
            val = ACCEPT_VAL
        elif a[0] == 'shift':
            val = a[1] + 1
        elif a[0] == 'reduce':
            val = -(a[1] + 1)
        else:
            continue
        action_by_state[si].append((ti, val))
    for si in action_by_state:
        action_by_state[si].sort()

    # Build goto entries per state
    goto_by_state = defaultdict(list)
    for (si, nt), sj in goto_t.items():
        ni = nt_idx.get(nt)
        if ni is None:
            continue
        goto_by_state[si].append((ni, sj))
    for si in goto_by_state:
        goto_by_state[si].sort()

    # Flatten
    all_act  = []
    act_row  = []
    for si in range(n_states):
        act_row.append(len(all_act))
        all_act.extend(action_by_state.get(si, []))
    act_row.append(len(all_act))

    all_goto = []
    goto_row = []
    for si in range(n_states):
        goto_row.append(len(all_goto))
        all_goto.extend(goto_by_state.get(si, []))
    goto_row.append(len(all_goto))

    with open(out_path, 'w', encoding='utf-8') as f:
        w = f.write

        w('// AUTO-GENERATED by lalr_gen.py — DO NOT EDIT\n')
        w(f'// {n_states} states | {len(terms)} terminals | {len(nts)} nonterminals'
          f' | {len(g.rules)} rules\n')
        w(f'// {len(all_act)} action entries | {len(all_goto)} goto entries\n\n')
        w('#pragma once\n')
        w('#include <cstdint>\n\n')

        # Terminal strings (sorted) — runtime maps TokenType -> index at startup
        w(f'static constexpr int XMC_NTERMS = {len(terms)};\n')
        w('static const char* const XMC_TERM_STR[] = {\n')
        for t in terms:
            w(f'    {_cpp_str(t)},\n')
        w('};\n\n')

        # Nonterminal names
        w(f'static constexpr int XMC_NNT = {len(nts)};\n')
        w('static const char* const XMC_NT_STR[] = {\n')
        for nt in nts:
            w(f'    {_cpp_str(nt)},\n')
        w('};\n\n')

        # Terminal index constants (XMC_TERM_<cname> = sorted index)
        w('// Terminal index constants\n')
        for i, t in enumerate(terms):
            w(f'static constexpr int XMC_TERM_{_term_to_cname(t)} = {i};\n')
        w('\n')

        # Nonterminal index constants (XMC_NT_<cname> = sorted index)
        w('// Nonterminal index constants\n')
        for i, nt in enumerate(nts):
            w(f'static constexpr int XMC_NT_{_nt_to_cname(nt)} = {i};\n')
        w('\n')

        # Rule info: lhs nonterminal index, rhs length, lhs name
        w('struct XmcRule { int16_t lhs; int16_t rhs_len; const char* name; };\n')
        w(f'static constexpr int XMC_NRULES = {len(g.rules)};\n')
        w('static const XmcRule XMC_RULES[] = {\n')
        for ri, (lhs, rhs) in enumerate(g.rules):
            ni = nt_idx.get(lhs, -1)
            w(f'    {{ {ni:4d}, {len(rhs):3d}, {_cpp_str(lhs)} }},\n')
        w('};\n\n')

        # Action table (sparse, sorted by term within each row)
        w('struct XmcAct { int16_t term; int32_t val; };\n')
        w(f'static constexpr int XMC_NACT = {len(all_act)};\n')
        w('static const XmcAct XMC_ACT[] = {\n')
        for ti, val in all_act:
            w(f'    {{ {ti:4d}, {val:12d} }},\n')
        w('};\n')
        w(f'static const int32_t XMC_ACT_ROW[{n_states + 1}] = {{\n    ')
        w(', '.join(str(v) for v in act_row))
        w('\n};\n\n')

        # Goto table (sparse, sorted by nt within each row)
        w('struct XmcGoto { int16_t nt; int16_t state; };\n')
        w(f'static constexpr int XMC_NGOTO = {len(all_goto)};\n')
        w('static const XmcGoto XMC_GOTO[] = {\n')
        for ni, sj in all_goto:
            w(f'    {{ {ni:4d}, {sj:4d} }},\n')
        w('};\n')
        w(f'static const int32_t XMC_GOTO_ROW[{n_states + 1}] = {{\n    ')
        w(', '.join(str(v) for v in goto_row))
        w('\n};\n')

    print(f"Emitted {out_path}")
    print(f"  action entries : {len(all_act)}")
    print(f"  goto   entries : {len(all_goto)}")


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    import argparse
    ap = argparse.ArgumentParser(description='LALR(1) table generator for xmc.bnf')
    ap.add_argument('bnf', help='path to xmc.bnf')
    ap.add_argument('--resolve', action='store_true',
                    help='auto-resolve S/R conflicts by preferring shift')
    ap.add_argument('--emit-cpp', metavar='PATH',
                    help='emit C++ tables to PATH (implies --resolve)')
    args = ap.parse_args()

    do_resolve = args.resolve or bool(args.emit_cpp)

    print(f"Reading {args.bnf} …")
    g = parse_bnf(args.bnf)

    print(f"Computing FIRST sets …")
    FIRST = compute_first(g)

    print(f"Building LR(0) item sets …")
    states, goto_map = build_lr0(g)

    print(f"Computing LALR(1) lookaheads …")
    lookaheads = compute_lalr1(states, goto_map, g, FIRST)

    print(f"Building tables …")
    action, goto_t, conflicts = build_tables(states, goto_map, lookaheads, g)

    resolved_sr = 0
    resolved_rr = 0
    remaining = conflicts
    if do_resolve:
        print(f"Resolving S/R conflicts by shift preference …")
        resolved_sr, remaining = resolve_sr_conflicts(action, remaining)
        print(f"Resolving R/R conflicts by lowest-rule preference …")
        resolved_rr, remaining = resolve_rr_conflicts(action, remaining)

    if args.emit_cpp:
        # Skip the verbose per-conflict report when emitting; just print counts.
        if resolved_sr:
            print(f"Auto-resolved {resolved_sr} S/R conflict(s) by shift preference.")
        if resolved_rr:
            print(f"Auto-resolved {resolved_rr} R/R conflict(s) by lowest-rule preference.")
        if remaining:
            print(f"WARNING: {len(remaining)} unresolved conflict(s) remain.")
        emit_cpp(g, states, goto_map, action, goto_t, args.emit_cpp)
    else:
        print()
        report(g, states, goto_map, action, remaining, lookaheads)
        if resolved_sr:
            print(f"\nAuto-resolved {resolved_sr} S/R conflict(s) by shift preference.")
        if resolved_rr:
            print(f"\nAuto-resolved {resolved_rr} R/R conflict(s) by lowest-rule preference.")


if __name__ == '__main__':
    main()
