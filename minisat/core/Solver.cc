/***************************************************************************************[Solver.cc]
Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
Copyright (c) 2007-2010, Niklas Sorensson

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#include <math.h>

#include "minisat/mtl/Alg.h"
#include "minisat/mtl/Sort.h"
#include "minisat/utils/System.h"
#include "minisat/core/Solver.h"

using namespace Minisat;

//=================================================================================================
// Options:


static const char* _cat = "CORE";

static DoubleOption  opt_var_decay         (_cat, "var-decay",   "The variable activity decay factor",            0.95,     DoubleRange(0, false, 1, false));
static DoubleOption  opt_clause_decay      (_cat, "cla-decay",   "The clause activity decay factor",              0.999,    DoubleRange(0, false, 1, false));
static DoubleOption  opt_random_var_freq   (_cat, "rnd-freq",    "The frequency with which the decision heuristic tries to choose a random variable", 0, DoubleRange(0, true, 1, true));
static DoubleOption  opt_random_seed       (_cat, "rnd-seed",    "Used by the random variable selection",         91648253, DoubleRange(0, false, HUGE_VAL, false));
static IntOption     opt_ccmin_mode        (_cat, "ccmin-mode",  "Controls conflict clause minimization (0=none, 1=basic, 2=deep)", 2, IntRange(0, 2));
static IntOption     opt_phase_saving      (_cat, "phase-saving", "Controls the level of phase saving (0=none, 1=limited, 2=full)", 2, IntRange(0, 2));
static BoolOption    opt_rnd_init_act      (_cat, "rnd-init",    "Randomize the initial activity", false);
static BoolOption    opt_luby_restart      (_cat, "luby",        "Use the Luby restart sequence", true);
static IntOption     opt_restart_first     (_cat, "rfirst",      "The base restart interval", 100, IntRange(1, INT32_MAX));
static DoubleOption  opt_restart_inc       (_cat, "rinc",        "Restart interval increase factor", 2, DoubleRange(1, false, HUGE_VAL, false));
static DoubleOption  opt_garbage_frac      (_cat, "gc-frac",     "The fraction of wasted memory allowed before a garbage collection is triggered",  0.20, DoubleRange(0, false, HUGE_VAL, false));
static IntOption     opt_min_learnts_lim   (_cat, "min-learnts", "Minimum learnt clause limit",  0, IntRange(0, INT32_MAX));
StringOption symmetry("SYMMETRY", "symm", "Permutations file.");
BoolOption symm_aux_decide("SYMMETRY", "symm-aux-decide", "Decide on symmetry added auxilary variables.", false);
BoolOption symm_break_shatter("SYMMETRY", "symm-shatter", "Break symmetries via emulating shatter.", false);
BoolOption symm_break_chaining_imp("SYMMETRY", "symm-chain", "Break symmetries via implication chaining SBPs", false);
BoolOption symm_eq_aux("SYMMETRY", "symm-eq-aux", "Use equality table auxiliary variables", false);
BoolOption symm_dynamic("SYMMETRY", "symm-dynamic", "Add the symmetry breaking clauses dynamically", false);

//=================================================================================================
// Constructor/Destructor:


Solver::Solver() :

    // Parameters (user settable):
    //
    verbosity        (0)
  , var_decay        (opt_var_decay)
  , clause_decay     (opt_clause_decay)
  , random_var_freq  (opt_random_var_freq)
  , random_seed      (opt_random_seed)
  , luby_restart     (opt_luby_restart)
  , ccmin_mode       (opt_ccmin_mode)
  , phase_saving     (opt_phase_saving)
  , rnd_pol          (false)
  , rnd_init_act     (opt_rnd_init_act)
  , garbage_frac     (opt_garbage_frac)
  , min_learnts_lim  (opt_min_learnts_lim)
  , restart_first    (opt_restart_first)
  , restart_inc      (opt_restart_inc)

    // Parameters (the rest):
    //
  , learntsize_factor((double)1/(double)3), learntsize_inc(1.1)

    // Parameters (experimental):
    //
  , learntsize_adjust_start_confl (100)
  , learntsize_adjust_inc         (1.5)

    // Statistics: (formerly in 'SolverStats')
    //
  , solves(0), starts(0), decisions(0), rnd_decisions(0), propagations(0), conflicts(0)
  , dec_vars(0), num_clauses(0), num_learnts(0), clauses_literals(0), learnts_literals(0), max_literals(0), tot_literals(0)

  , watches            (WatcherDeleted(ca))
  , order_heap         (VarOrderLt(activity))
  , ok                 (true)
  , cla_inc            (1)
  , var_inc            (1)
  , qhead              (0)
  , simpDB_assigns     (-1)
  , simpDB_props       (0)
  , progress_estimate  (0)
  , remove_satisfied   (true)
  , next_var           (0)

    // Resource constraints:
    //
  , conflict_budget    (-1)
  , propagation_budget (-1)
  , asynch_interrupt   (false)
  , NumNaiveEqs(0)
  , NumEqs(0)
{}


Solver::~Solver()
{
  if(symm_dynamic){
    int i = 0 ; 
    for(i = 1 ; i < this->orig_vars ; i++){
      free(this->watchedEqs[i]);
    }        
  free(this->watchedEqs);
  }
}


//=================================================================================================
// Minor methods:


// Creates a new SAT variable in the solver. If 'decision' is cleared, variable will not be
// used as a decision variable (NOTE! This has effects on the meaning of a SATISFIABLE result).
//
Var Solver::newVar(lbool upol, bool dvar)
{
    Var v;
    if (free_vars.size() > 0){
        v = free_vars.last();
        free_vars.pop();
    }else
        v = next_var++;

    watches  .init(mkLit(v, false));
    watches  .init(mkLit(v, true ));
    assigns  .insert(v, l_Undef);
    vardata  .insert(v, mkVarData(CRef_Undef, 0));
    activity .insert(v, rnd_init_act ? drand(random_seed) * 0.00001 : 0);
    seen     .insert(v, 0);
    polarity .insert(v, true);
    user_pol .insert(v, upol);
    decision .reserve(v);
    trail    .capacity(v+1);
    setDecisionVar(v, dvar);
    return v;
}

Var Solver::newSymmAuxVar() {
    Var v = Solver::newVar(l_Undef, symm_aux_decide);
    return v;
}

// Note: at the moment, only unassigned variable will be released (this is to avoid duplicate
// releases of the same variable).
void Solver::releaseVar(Lit l)
{
    if (value(l) == l_Undef){
      addClause(l, false);
        released_vars.push(var(l));
    }
}


bool Solver::addClause_(vec<Lit>& ps, bool SBP)
{
    if(!SBP)
      assert(decisionLevel() == 0);
    if (!ok) return false;

    // Check if clause is satisfied and remove false/duplicate literals:
    sort(ps);
    Lit p; int i, j;
    for (i = j = 0, p = lit_Undef; i < ps.size(); i++)
        if (value(ps[i]) == l_True || ps[i] == ~p)
            return true;
        else if (value(ps[i]) != l_False && ps[i] != p)
            ps[j++] = p = ps[i];
    ps.shrink(i - j);

    if (ps.size() == 0)
        return ok = false;
    else if (ps.size() == 1){
        uncheckedEnqueue(ps[0]);
        return ok = (propagate() == CRef_Undef);
    }else{
        CRef cr = ca.alloc(ps, false);
        Clause& c = ca[cr];
        c.setIsSBP(SBP);
        c.setPropagated(0);
        c.setResAnal(0);
        clauses.push(cr);
        attachClause(cr);
    }

    return true;
}


void Solver::attachClause(CRef cr){
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    watches[~c[0]].push(Watcher(cr, c[1]));
    watches[~c[1]].push(Watcher(cr, c[0]));
    if (c.learnt()) num_learnts++, learnts_literals += c.size();
    else            num_clauses++, clauses_literals += c.size();
}


void Solver::detachClause(CRef cr, bool strict){
    const Clause& c = ca[cr];
    assert(c.size() > 1);
    
    // Strict or lazy detaching:
    if (strict){
        remove(watches[~c[0]], Watcher(cr, c[1]));
        remove(watches[~c[1]], Watcher(cr, c[0]));
    }else{
        watches.smudge(~c[0]);
        watches.smudge(~c[1]);
    }

    if (c.learnt()) num_learnts--, learnts_literals -= c.size();
    else            num_clauses--, clauses_literals -= c.size();
}


void Solver::removeClause(CRef cr) {
    Clause& c = ca[cr];
    detachClause(cr);
    // Don't leave pointers to free'd memory!
    if (locked(c)) vardata[var(c[0])].reason = CRef_Undef;
    c.mark(1); 
    ca.free(cr);
}


bool Solver::satisfied(const Clause& c) const {
    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) == l_True)
            return true;
    return false; }


// Revert to the state at given level (keeping all assignment at 'level' but not beyond).
//
void Solver::cancelUntil(int level) {
    if (decisionLevel() > level){
        for (int c = trail.size()-1; c >= trail_lim[level]; c--){
            Var      x  = var(trail[c]);
            assigns [x] = l_Undef;
            if (phase_saving > 1 || (phase_saving == 1 && c > trail_lim.last()))
                polarity[x] = sign(trail[c]);
            insertVarOrder(x); }
        qhead = trail_lim[level];
        trail.shrink(trail.size() - trail_lim[level]);
        trail_lim.shrink(trail_lim.size() - level);
    } }


//=================================================================================================
// Major methods:


Lit Solver::pickBranchLit()
{
    Var next = var_Undef;

    // Random decision:
    if (drand(random_seed) < random_var_freq && !order_heap.empty()){
        next = order_heap[irand(random_seed,order_heap.size())];
        if (value(next) == l_Undef && decision[next])
            rnd_decisions++; }

    // Activity based decision:
    while (next == var_Undef || value(next) != l_Undef || !decision[next])
        if (order_heap.empty()){
            next = var_Undef;
            break;
        }else
            next = order_heap.removeMin();

    // Choose polarity based on different polarity modes (global or per-variable):
    if (next == var_Undef)
        return lit_Undef;
    else if (user_pol[next] != l_Undef)
        return mkLit(next, user_pol[next] == l_True);
    else if (rnd_pol)
        return mkLit(next, drand(random_seed) < 0.5);
    else
        return mkLit(next, polarity[next]);
}


/*_________________________________________________________________________________________________
|
|  analyze : (confl : Clause*) (out_learnt : vec<Lit>&) (out_btlevel : int&)  ->  [void]
|  
|  Description:
|    Analyze conflict and produce a reason clause.
|  
|    Pre-conditions:
|      * 'out_learnt' is assumed to be cleared.
|      * Current decision level must be greater than root level.
|  
|    Post-conditions:
|      * 'out_learnt[0]' is the asserting literal at level 'out_btlevel'.
|      * If out_learnt.size() > 1 then 'out_learnt[1]' has the greatest decision level of the 
|        rest of literals. There may be others from the same level though.
|  
|________________________________________________________________________________________________@*/
void Solver::analyze(CRef confl, vec<Lit>& out_learnt, int& out_btlevel)
{
    int pathC = 0;
    Lit p     = lit_Undef;

    // Generate conflict clause:
    //
    out_learnt.push();      // (leave room for the asserting literal)
    int index   = trail.size() - 1;

    do{
        assert(confl != CRef_Undef); // (otherwise should be UIP)
        Clause& c = ca[confl];
        c.setResAnal(true);
        if (c.learnt())
            claBumpActivity(c);

        for (int j = (p == lit_Undef) ? 0 : 1; j < c.size(); j++){
            Lit q = c[j];

            if (!seen[var(q)] && level(var(q)) > 0){
                varBumpActivity(var(q));
                seen[var(q)] = 1;
                if (level(var(q)) >= decisionLevel())
                    pathC++;
                else
                    out_learnt.push(q);
            }
        }
        
        // Select next clause to look at:
        while (!seen[var(trail[index--])]);
        p     = trail[index+1];
        confl = reason(var(p));
        seen[var(p)] = 0;
        pathC--;

    }while (pathC > 0);
    out_learnt[0] = ~p;

    // Simplify conflict clause:
    //
    int i, j;
    out_learnt.copyTo(analyze_toclear);
    if (ccmin_mode == 2){
        for (i = j = 1; i < out_learnt.size(); i++)
            if (reason(var(out_learnt[i])) == CRef_Undef || !litRedundant(out_learnt[i]))
                out_learnt[j++] = out_learnt[i];
        
    }else if (ccmin_mode == 1){
        for (i = j = 1; i < out_learnt.size(); i++){
            Var x = var(out_learnt[i]);

            if (reason(x) == CRef_Undef)
                out_learnt[j++] = out_learnt[i];
            else{
                Clause& c = ca[reason(var(out_learnt[i]))];
                c.setResAnal(true);
                for (int k = 1; k < c.size(); k++)
                    if (!seen[var(c[k])] && level(var(c[k])) > 0){
                        out_learnt[j++] = out_learnt[i];
                        break; }
            }
        }
    }else
        i = j = out_learnt.size();

    max_literals += out_learnt.size();
    out_learnt.shrink(i - j);
    tot_literals += out_learnt.size();

    // Find correct backtrack level:
    //
    if (out_learnt.size() == 1)
        out_btlevel = 0;
    else{
        int max_i = 1;
        // Find the first literal assigned at the next-highest level:
        for (int i = 2; i < out_learnt.size(); i++)
            if (level(var(out_learnt[i])) > level(var(out_learnt[max_i])))
                max_i = i;
        // Swap-in this literal at index 1:
        Lit p             = out_learnt[max_i];
        out_learnt[max_i] = out_learnt[1];
        out_learnt[1]     = p;
        out_btlevel       = level(var(p));
    }

    for (int j = 0; j < analyze_toclear.size(); j++) seen[var(analyze_toclear[j])] = 0;    // ('seen[]' is now cleared)
}


// Check if 'p' can be removed from a conflict clause.
bool Solver::litRedundant(Lit p)
{
    enum { seen_undef = 0, seen_source = 1, seen_removable = 2, seen_failed = 3 };
    assert(seen[var(p)] == seen_undef || seen[var(p)] == seen_source);
    assert(reason(var(p)) != CRef_Undef);

    Clause*               c     = &ca[reason(var(p))];
    vec<ShrinkStackElem>& stack = analyze_stack;
    stack.clear();

    for (uint32_t i = 1; ; i++){
        if (i < (uint32_t)c->size()){
            // Checking 'p'-parents 'l':
            Lit l = (*c)[i];
            
            // Variable at level 0 or previously removable:
            if (level(var(l)) == 0 || seen[var(l)] == seen_source || seen[var(l)] == seen_removable){
                continue; }
            
            // Check variable can not be removed for some local reason:
            if (reason(var(l)) == CRef_Undef || seen[var(l)] == seen_failed){
                stack.push(ShrinkStackElem(0, p));
                for (int i = 0; i < stack.size(); i++)
                    if (seen[var(stack[i].l)] == seen_undef){
                        seen[var(stack[i].l)] = seen_failed;
                        analyze_toclear.push(stack[i].l);
                    }
                    
                return false;
            }

            // Recursively check 'l':
            stack.push(ShrinkStackElem(i, p));
            i  = 0;
            p  = l;
            c  = &ca[reason(var(p))];
        }else{
            // Finished with current element 'p' and reason 'c':
            if (seen[var(p)] == seen_undef){
                seen[var(p)] = seen_removable;
                analyze_toclear.push(p);
            }

            // Terminate with success if stack is empty:
            if (stack.size() == 0) break;
            
            // Continue with top element on stack:
            i  = stack.last().i;
            p  = stack.last().l;
            c  = &ca[reason(var(p))];

            stack.pop();
        }
    }

    return true;
}


/*_________________________________________________________________________________________________
|
|  analyzeFinal : (p : Lit)  ->  [void]
|  
|  Description:
|    Specialized analysis procedure to express the final conflict in terms of assumptions.
|    Calculates the (possibly empty) set of assumptions that led to the assignment of 'p', and
|    stores the result in 'out_conflict'.
|________________________________________________________________________________________________@*/
void Solver::analyzeFinal(Lit p, LSet& out_conflict)
{
    out_conflict.clear();
    out_conflict.insert(p);

    if (decisionLevel() == 0)
        return;

    seen[var(p)] = 1;

    for (int i = trail.size()-1; i >= trail_lim[0]; i--){
        Var x = var(trail[i]);
        if (seen[x]){
            if (reason(x) == CRef_Undef){
                assert(level(x) > 0);
                out_conflict.insert(~trail[i]);
            }else{
                Clause& c = ca[reason(x)];
                for (int j = 1; j < c.size(); j++)
                    if (level(var(c[j])) > 0)
                        seen[var(c[j])] = 1;
            }
            seen[x] = 0;
        }
    }

    seen[var(p)] = 0;
}


void Solver::uncheckedEnqueue(Lit p, CRef from)
{
    // TODO: assignement happens here
    assert(value(p) == l_Undef);
    assigns[var(p)] = lbool(!sign(p));
    vardata[var(p)] = mkVarData(from, decisionLevel());
    trail.push_(p);
    for(unsigned int permIdx = 0 ; permIdx < this->nSymmetries ; permIdx++)
      {
        //printf("var(p)=%d\n",var(p) + 1);
        if(var(p) < this->orig_vars)
          if(watchedEqs[var(p) + 1][permIdx] != NULL)
            if(predSat(watchedEqs[var(p) + 1][permIdx], permIdx))
              /*printf("Eq satisfied\n"),*/ addSucc(watchedEqs[var(p) + 1][permIdx], permIdx);
      }
}


/*_________________________________________________________________________________________________
|
|  propagate : [void]  ->  [Clause*]
|  
|  Description:
|    Propagates all enqueued facts. If a conflict arises, the conflicting clause is returned,
|    otherwise CRef_Undef.
|  
|    Post-conditions:
|      * the propagation queue is empty, even if there was a conflict.
|________________________________________________________________________________________________@*/
CRef Solver::propagate()
{
    CRef    confl     = CRef_Undef;
    int     num_props = 0;

    while (qhead < trail.size()){
        Lit            p   = trail[qhead++];     // 'p' is enqueued fact to propagate.
        vec<Watcher>&  ws  = watches.lookup(p);
        Watcher        *i, *j, *end;
        num_props++;

        for (i = j = (Watcher*)ws, end = i + ws.size();  i != end;){
            // Try to avoid inspecting the clause:
            Lit blocker = i->blocker;
            if (value(blocker) == l_True){
                *j++ = *i++; continue; }

            // Make sure the false literal is data[1]:
            CRef     cr        = i->cref;
            Clause&  c         = ca[cr];
            //printf("Propagating clause %d\n", cr);
            ca[cr].setPropagated(true);
	      //  ca[cr].firstPropagation = this->conflicts;
            // ca[cr].numPropagations++;
            Lit      false_lit = ~p;
            if (c[0] == false_lit)
                c[0] = c[1], c[1] = false_lit;
            assert(c[1] == false_lit);
            i++;

            // If 0th watch is true, then clause is already satisfied.
            Lit     first = c[0];
            Watcher w     = Watcher(cr, first);
            if (first != blocker && value(first) == l_True){
                *j++ = w; continue; }

            // Look for new watch:
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) != l_False){
                    c[1] = c[k]; c[k] = false_lit;
                    watches[~c[1]].push(w);
                    goto NextClause; }

            // Did not find watch -- clause is unit under assignment:
            *j++ = w;
            if (value(first) == l_False){
                confl = cr;
                qhead = trail.size();
                // Copy the remaining watches:
                while (i < end)
                    *j++ = *i++;
            }else
                uncheckedEnqueue(first, cr);

        NextClause:;
        }
        ws.shrink(i - j);
    }
    propagations += num_props;
    simpDB_props -= num_props;

    return confl;
}


/*_________________________________________________________________________________________________
|
|  reduceDB : ()  ->  [void]
|  
|  Description:
|    Remove half of the learnt clauses, minus the clauses locked by the current assignment. Locked
|    clauses are clauses that are reason to some assignment. Binary clauses are never removed.
|________________________________________________________________________________________________@*/
struct reduceDB_lt { 
    ClauseAllocator& ca;
    reduceDB_lt(ClauseAllocator& ca_) : ca(ca_) {}
    bool operator () (CRef x, CRef y) { 
        return ca[x].size() > 2 && (ca[y].size() == 2 || ca[x].activity() < ca[y].activity()); } 
};
void Solver::reduceDB()
{
    int     i, j;
    double  extra_lim = cla_inc / learnts.size();    // Remove any clause below this activity

    sort(learnts, reduceDB_lt(ca));
    // Don't delete binary or locked clauses. From the rest, delete clauses from the first half
    // and clauses with activity smaller than 'extra_lim':
    for (i = j = 0; i < learnts.size(); i++){
        Clause& c = ca[learnts[i]];
        if (c.size() > 2 && !locked(c) && (i < learnts.size() / 2 || c.activity() < extra_lim))
            removeClause(learnts[i]);
        else
            learnts[j++] = learnts[i];
    }
    learnts.shrink(i - j);
    checkGarbage();
}


void Solver::removeSatisfied(vec<CRef>& cs)
{
    int i, j;
    for (i = j = 0; i < cs.size(); i++){
        Clause& c = ca[cs[i]];
        if (satisfied(c))
            removeClause(cs[i]);
        else{
            // Trim clause:
            assert(value(c[0]) == l_Undef && value(c[1]) == l_Undef);
            for (int k = 2; k < c.size(); k++)
                if (value(c[k]) == l_False){
                    c[k--] = c[c.size()-1];
                    c.pop();
                }
            cs[j++] = cs[i];
        }
    }
    cs.shrink(i - j);
}


void Solver::rebuildOrderHeap()
{
    vec<Var> vs;
    for (Var v = 0; v < nVars(); v++)
        if (decision[v] && value(v) == l_Undef)
            vs.push(v);
    order_heap.build(vs);
}


/*_________________________________________________________________________________________________
|
|  simplify : [void]  ->  [bool]
|  
|  Description:
|    Simplify the clause database according to the current top-level assigment. Currently, the only
|    thing done here is the removal of satisfied clauses, but more things can be put here.
|________________________________________________________________________________________________@*/
bool Solver::simplify()
{
    assert(decisionLevel() == 0);

    if (!ok || propagate() != CRef_Undef)
        return ok = false;

    if (nAssigns() == simpDB_assigns || (simpDB_props > 0))
        return true;

    // Remove satisfied clauses:
    removeSatisfied(learnts);
    if (remove_satisfied){       // Can be turned off.
        removeSatisfied(clauses);

        // TODO: what todo in if 'remove_satisfied' is false?

        // Remove all released variables from the trail:
        for (int i = 0; i < released_vars.size(); i++){
            assert(seen[released_vars[i]] == 0);
            seen[released_vars[i]] = 1;
        }

        int i, j;
        for (i = j = 0; i < trail.size(); i++)
            if (seen[var(trail[i])] == 0)
                trail[j++] = trail[i];
        trail.shrink(i - j);
        //printf("trail.size()= %d, qhead = %d\n", trail.size(), qhead);
        qhead = trail.size();

        for (int i = 0; i < released_vars.size(); i++)
            seen[released_vars[i]] = 0;

        // Released variables are now ready to be reused:
        append(released_vars, free_vars);
        released_vars.clear();
    }
    checkGarbage();
    rebuildOrderHeap();

    simpDB_assigns = nAssigns();
    simpDB_props   = clauses_literals + learnts_literals;   // (shouldn't depend on stats really, but it will do for now)

    return true;
}


/*_________________________________________________________________________________________________
|
|  search : (nof_conflicts : int) (params : const SearchParams&)  ->  [lbool]
|  
|  Description:
|    Search for a model the specified number of conflicts. 
|    NOTE! Use negative value for 'nof_conflicts' indicate infinity.
|  
|  Output:
|    'l_True' if a partial assigment that is consistent with respect to the clauseset is found. If
|    all variables are decision variables, this means that the clause set is satisfiable. 'l_False'
|    if the clause set is unsatisfiable. 'l_Undef' if the bound on number of conflicts is reached.
|________________________________________________________________________________________________@*/
lbool Solver::search(int nof_conflicts)
{
    assert(ok);
    int         backtrack_level;
    int         conflictC = 0;
    vec<Lit>    learnt_clause;
    starts++;

    for (;;){
        CRef confl = propagate();
        if (confl != CRef_Undef){
            // CONFLICT
            conflicts++; conflictC++;
            if (decisionLevel() == 0) return l_False;

            learnt_clause.clear();
            analyze(confl, learnt_clause, backtrack_level);
            cancelUntil(backtrack_level);

            if (learnt_clause.size() == 1){
                uncheckedEnqueue(learnt_clause[0]);
            }else{
                CRef cr = ca.alloc(learnt_clause, true);
                ca[cr].setIsSBP(false);
                ca[cr].setPropagated(0);
                ca[cr].setResAnal(0);
                learnts.push(cr);
                attachClause(cr);
                claBumpActivity(ca[cr]);
                uncheckedEnqueue(learnt_clause[0], cr);
            }

            varDecayActivity();
            claDecayActivity();

            if (--learntsize_adjust_cnt == 0){
                learntsize_adjust_confl *= learntsize_adjust_inc;
                learntsize_adjust_cnt    = (int)learntsize_adjust_confl;
                max_learnts             *= learntsize_inc;

                if (verbosity >= 1)
                    printf("| %9d | %7d %8d %8d | %8d %8d %6.0f | %6.3f %% |\n", 
                           (int)conflicts, 
                           (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : trail_lim[0]), nClauses(), (int)clauses_literals, 
                           (int)max_learnts, nLearnts(), (double)learnts_literals/nLearnts(), progressEstimate()*100);
            }

        }else{
            // NO CONFLICT
            if ((nof_conflicts >= 0 && conflictC >= nof_conflicts) || !withinBudget()){
                // Reached bound on number of conflicts:
                progress_estimate = progressEstimate();
                cancelUntil(0);
                return l_Undef; }

            // Simplify the set of problem clauses:
            if (decisionLevel() == 0 && !simplify())
                return l_False;

            if (learnts.size()-nAssigns() >= max_learnts)
                // Reduce the set of learnt clauses:
                reduceDB();

            Lit next = lit_Undef;
            while (decisionLevel() < assumptions.size()){
                // Perform user provided assumption:
                Lit p = assumptions[decisionLevel()];
                if (value(p) == l_True){
                    // Dummy decision level:
                    newDecisionLevel();
                }else if (value(p) == l_False){
                    analyzeFinal(~p, conflict);
                    return l_False;
                }else{
                    next = p;
                    break;
                }
            }

            if (next == lit_Undef){
                // New variable decision:
                decisions++;
                next = pickBranchLit();

                if (next == lit_Undef)
                    // Model found:
                    return l_True;
            }
            //TODO: assignement happens here 1

            // Increase decision level and enqueue 'next'
            newDecisionLevel();
            uncheckedEnqueue(next);
        }
    }
}


double Solver::progressEstimate() const
{
    double  progress = 0;
    double  F = 1.0 / nVars();

    for (int i = 0; i <= decisionLevel(); i++){
        int beg = i == 0 ? 0 : trail_lim[i - 1];
        int end = i == decisionLevel() ? trail.size() : trail_lim[i];
        progress += pow(F, i) * (end - beg);
    }

    return progress / nVars();
}

/*
  Finite subsequences of the Luby-sequence:

  0: 1
  1: 1 1 2
  2: 1 1 2 1 1 2 4
  3: 1 1 2 1 1 2 4 1 1 2 1 1 2 4 8
  ...


 */

static double luby(double y, int x){

    // Find the finite subsequence that contains index 'x', and the
    // size of that subsequence:
    int size, seq;
    for (size = 1, seq = 0; size < x+1; seq++, size = 2*size+1);

    while (size-1 != x){
        size = (size-1)>>1;
        seq--;
        x = x % size;
    }

    return pow(y, seq);
}

// NOTE: assumptions passed in member-variable 'assumptions'.
lbool Solver::solve_()
{
    model.clear();
    conflict.clear();
    if (!ok) return l_False;

    solves++;

    max_learnts = nClauses() * learntsize_factor;
    if (max_learnts < min_learnts_lim)
        max_learnts = min_learnts_lim;

    learntsize_adjust_confl   = learntsize_adjust_start_confl;
    learntsize_adjust_cnt     = (int)learntsize_adjust_confl;
    lbool   status            = l_Undef;

    if (verbosity >= 1){
        printf("============================[ Search Statistics ]==============================\n");
        printf("| Conflicts |          ORIGINAL         |          LEARNT          | Progress |\n");
        printf("|           |    Vars  Clauses Literals |    Limit  Clauses Lit/Cl |          |\n");
        printf("===============================================================================\n");
    }

    // Search:
    int curr_restarts = 0;
    while (status == l_Undef){
        double rest_base = luby_restart ? luby(restart_inc, curr_restarts) : pow(restart_inc, curr_restarts);
        status = search(rest_base * restart_first);
        if (!withinBudget()) break;
        curr_restarts++;
    }

    if (verbosity >= 1)
        printf("===============================================================================\n");


    if (status == l_True){
        // Extend & copy model:
        model.growTo(nVars());
        for (int i = 0; i < nVars(); i++) model[i] = value(i);
    }else if (status == l_False && conflict.size() == 0)
        ok = false;

    cancelUntil(0);
    return status;
}


bool Solver::implies(const vec<Lit>& assumps, vec<Lit>& out)
{
    trail_lim.push(trail.size());
    for (int i = 0; i < assumps.size(); i++){
        Lit a = assumps[i];

        if (value(a) == l_False){
            cancelUntil(0);
            return false;
        }else if (value(a) == l_Undef)
            uncheckedEnqueue(a);
    }

    unsigned trail_before = trail.size();
    bool     ret          = true;
    if (propagate() == CRef_Undef){
        out.clear();
        for (int j = trail_before; j < trail.size(); j++)
            out.push(trail[j]);
    }else
        ret = false;
    
    cancelUntil(0);
    return ret;
}

//=================================================================================================
// Writing CNF to DIMACS:
// 
// FIXME: this needs to be rewritten completely.

static Var mapVar(Var x, vec<Var>& map, Var& max)
{
    if (map.size() <= x || map[x] == -1){
        map.growTo(x+1, -1);
        map[x] = max++;
    }
    return map[x];
}


void Solver::toDimacs(FILE* f, Clause& c, vec<Var>& map, Var& max)
{
    if (satisfied(c)) return;

    for (int i = 0; i < c.size(); i++)
        if (value(c[i]) != l_False)
            fprintf(f, "%s%d ", sign(c[i]) ? "-" : "", mapVar(var(c[i]), map, max)+1);
    fprintf(f, "0\n");
}


void Solver::toDimacs(const char *file, const vec<Lit>& assumps)
{
    FILE* f = fopen(file, "wr");
    if (f == NULL)
        fprintf(stderr, "could not open file %s\n", file), exit(1);
    toDimacs(f, assumps);
    fclose(f);
}


void Solver::toDimacs(FILE* f, const vec<Lit>& assumps)
{
    // Handle case when solver is in contradictory state:
    if (!ok){
        fprintf(f, "p cnf 1 2\n1 0\n-1 0\n");
        return; }

    vec<Var> map; Var max = 0;

    // Cannot use removeClauses here because it is not safe
    // to deallocate them at this point. Could be improved.
    int cnt = 0;
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]]))
            cnt++;
        
    for (int i = 0; i < clauses.size(); i++)
        if (!satisfied(ca[clauses[i]])){
            Clause& c = ca[clauses[i]];
            for (int j = 0; j < c.size(); j++)
                if (value(c[j]) != l_False)
                    mapVar(var(c[j]), map, max);
        }

    // Assumptions are added as unit clauses:
    cnt += assumps.size();

    fprintf(f, "p cnf %d %d\n", max, cnt);

    for (int i = 0; i < assumps.size(); i++){
        assert(value(assumps[i]) != l_False);
        fprintf(f, "%s%d 0\n", sign(assumps[i]) ? "-" : "", mapVar(var(assumps[i]), map, max)+1);
    }

    for (int i = 0; i < clauses.size(); i++)
        toDimacs(f, ca[clauses[i]], map, max);

    if (verbosity > 0)
        printf("Wrote DIMACS with %d variables and %d clauses.\n", max, cnt);
}


void Solver::printStats() const
{
    double cpu_time = cpuTime();
    double mem_used = memUsedPeak();
    printf("restarts              : %"PRIu64"\n", starts);
    printf("conflicts             : %-12"PRIu64"   (%.0f /sec)\n", conflicts   , conflicts   /cpu_time);
    printf("decisions             : %-12"PRIu64"   (%4.2f %% random) (%.0f /sec)\n", decisions, (float)rnd_decisions*100 / (float)decisions, decisions   /cpu_time);
    printf("propagations          : %-12"PRIu64"   (%.0f /sec)\n", propagations, propagations/cpu_time);
    printf("conflict literals     : %-12"PRIu64"   (%4.2f %% deleted)\n", tot_literals, (max_literals - tot_literals)*100 / (double)max_literals);
    if (mem_used != 0) printf("Memory used           : %.2f MB\n", mem_used);
    printf("CPU time              : %g s\n", cpu_time);
}


//=================================================================================================
// Garbage Collection methods:

void Solver::relocAll(ClauseAllocator& to)
{
    // All watchers:
    //
    watches.cleanAll();
    for (int v = 0; v < nVars(); v++)
        for (int s = 0; s < 2; s++){
            Lit p = mkLit(v, s);
            vec<Watcher>& ws = watches[p];
            for (int j = 0; j < ws.size(); j++)
                ca.reloc(ws[j].cref, to);
        }

    // All reasons:
    //
    for (int i = 0; i < trail.size(); i++){
        Var v = var(trail[i]);

        // Note: it is not safe to call 'locked()' on a relocated clause. This is why we keep
        // 'dangling' reasons here. It is safe and does not hurt.
        if (reason(v) != CRef_Undef && (ca[reason(v)].reloced() || locked(ca[reason(v)]))){
            assert(!isRemoved(reason(v)));
            ca.reloc(vardata[v].reason, to);
        }
    }

    // All learnt:
    //
    int i, j;
    for (i = j = 0; i < learnts.size(); i++)
        if (!isRemoved(learnts[i])){
            ca.reloc(learnts[i], to);
            learnts[j++] = learnts[i];
        }
    learnts.shrink(i - j);

    // All original:
    //
    for (i = j = 0; i < clauses.size(); i++)
        if (!isRemoved(clauses[i])){
            ca.reloc(clauses[i], to);
            clauses[j++] = clauses[i];
        }
    clauses.shrink(i - j);
}


void Solver::garbageCollect()
{
    // Initialize the next region to a size corresponding to the estimated utilization degree. This
    // is not precise but should avoid some unnecessary reallocations for the new region:
    ClauseAllocator to(ca.size() - ca.wasted()); 

    relocAll(to);
    if (verbosity >= 2)
        printf("|  Garbage collection:   %12d bytes => %12d bytes             |\n", 
               ca.size()*ClauseAllocator::Unit_Size, to.size()*ClauseAllocator::Unit_Size);
    to.moveTo(ca);
}

struct cycle_lt {
  bool operator () (const vec<Lit>& x, const vec<Lit>& y) {
    return x[0] < y[0];
  }
};

struct cycle_swap {
  void operator () (vec<Lit>& x, vec<Lit>& y) {
    x.swapWith(y);
  }
};

bool Solver::addSymmetryGenerator(vec<vec<Lit> >& generator) {

  if (!ok) {
    return ok;
  }

  // Normalize the generator to the form
  //   G = c_1 c_2 ... c_m
  // where each cycle
  //   c_i = (l_i,1 l_i,2 ... l_i,ni)
  //   l_i,1 < l_i,2 ... l_i,ni
  // and
  //   l_i,1 < l_j,1 for i < j
  //
  // In adition since c_i and it's (Boolan) negation are semantically the same
  // we only keep the versions of the cycles that start with a positive variable

  // Normalize each cycle
  int gen_i;
  for (gen_i = 0; gen_i < generator.size(); ++ gen_i) {
    // The cycle
    vec<Lit>& cycle = generator[gen_i];
    assert(cycle.size() > 1);
    // Get the smallest one
    int cycle_i, min_cycle_i = 0;
    for (cycle_i = 1; cycle_i < cycle.size(); ++ cycle_i) {
      if (cycle[cycle_i] < cycle[min_cycle_i]) {
        min_cycle_i = cycle_i;
      }
    }
    // Rotate
    cycle.rotate(min_cycle_i);
    // Normalize
    if (sign(cycle[0])) {
      for (cycle_i = 0; cycle_i < cycle.size(); ++ cycle_i) {
        cycle[cycle_i] = ~cycle[cycle_i];
      }
    }
  }

  // Now sort the cycles based on first elements
  sort<vec<Lit>, cycle_lt, cycle_swap>(generator, cycle_lt(), cycle_swap());

  // Compact by removing duplicates
  int keep = 1;
  for (gen_i = 1; gen_i < generator.size(); ++ gen_i) {
    const vec<Lit>& prev = generator[keep-1];
    const vec<Lit>& current = generator[gen_i];
    // Enough to check first two
    if (prev[0] != current[0]) {
      generator[keep++].swapWith(generator[gen_i]);
    } else {
#ifndef NDEBUG
      assert(prev.size() == current.size());
      int cycle_i;
      for (cycle_i = 0; cycle_i < prev.size(); ++ cycle_i) {
        assert(prev[cycle_i] == current[cycle_i]);
      }
#endif
    }
  }
  generator.shrink(gen_i - keep);

  return ok;
}

void Solver::printSBPStats()
{
  int i = 0 ;
  int numSBPs = 0;
  int untouchedSBP = 0;
  int unResAnalSBP = 0;

  int numNoSBPs = 0;
  int untouchedNoSBP = 0;
  int unResAnalNoSBP = 0;
  int minSBPFirstPropagation = 1000000000;
  int avgSBPFirstPropagation = 0;
  int maxSBPFirstPropagation = -1;
  unsigned int minSBPNumPropagation = 0;
  unsigned int avgSBPNumPropagation = 0;
  unsigned int maxSBPNumPropagation = 0;
  int minNoSBPFirstPropagation = 1000000000;
  int avgNoSBPFirstPropagation = 0;
  int maxNoSBPFirstPropagation = -1;
  unsigned int minNoSBPNumPropagation = 0;
  unsigned int avgNoSBPNumPropagation = 0;
  unsigned int maxNoSBPNumPropagation = 0;
  for( i = 0 ; i < this->clauses.size() ; i++)
    {
      if (ca[clauses[i]].getIsSBP())
        {
          if(!ca[clauses[i]].getPropagated())
            untouchedSBP++;
          if(!ca[clauses[i]].getResAnal())
            unResAnalSBP++;
          // if(ca[clauses[i]].firstPropagation <  minSBPFirstPropagation)
          //   minSBPFirstPropagation = ca[clauses[i]].firstPropagation;
          // avgSBPFirstPropagation += ca[clauses[i]].firstPropagation;
          // if(ca[clauses[i]].firstPropagation >  maxSBPFirstPropagation)
          //   maxSBPFirstPropagation = ca[clauses[i]].firstPropagation;                
          // if(ca[clauses[i]].numPropagations <  minSBPNumPropagation)
          //   minSBPNumPropagation = ca[clauses[i]].numPropagations;
          // avgSBPNumPropagation += ca[clauses[i]].numPropagations;
          // if(ca[clauses[i]].numPropagations >  maxSBPNumPropagation)
          //   maxSBPNumPropagation = ca[clauses[i]].numPropagations;
          numSBPs++;
        }
      else
        {              
          if(!ca[clauses[i]].getPropagated())
            untouchedNoSBP++;
          if(!ca[clauses[i]].getResAnal())
            unResAnalNoSBP++;
          // if(ca[clauses[i]].firstPropagation <  minNoSBPFirstPropagation)
          //   minNoSBPFirstPropagation = ca[clauses[i]].firstPropagation;
          // avgNoSBPFirstPropagation += ca[clauses[i]].firstPropagation;
          // if(ca[clauses[i]].firstPropagation >  maxNoSBPFirstPropagation)
          //   maxNoSBPFirstPropagation = ca[clauses[i]].firstPropagation;                
          // if(ca[clauses[i]].numPropagations <  minNoSBPNumPropagation)
          //   minNoSBPNumPropagation = ca[clauses[i]].numPropagations;
          // avgNoSBPNumPropagation += ca[clauses[i]].numPropagations;
          // if(ca[clauses[i]].numPropagations >  maxNoSBPNumPropagation)
          //   maxNoSBPNumPropagation = ca[clauses[i]].numPropagations;
          numNoSBPs++;
        }
    }
  printf(\
  "  NumSBP = %d\n\
  untouchedSBP = %d\n\
  unResAnalSBP = %d\n\
  NumNoSBP = %d\n\
  untouchedNoSBP = %d\n\
  unResAnalNoSBP = %d\n",\
  numSBPs,\
  untouchedSBP,\
  unResAnalSBP,\
  numNoSBPs,\
  untouchedNoSBP,\
  unResAnalNoSBP);
  printf("clauses.size() = %d num_clauses=%d, \n", clauses.size(), nClauses());
}

int Solver::addInitShatterSBP(unsigned int x0, int f_x0){
  this->newSymmAuxVar();
  int p0 = this->nVars() - 1;
  if(symm_eq_aux)
    {
      unsigned int eqAuxVarID = this->addEqAuxVars(x0, f_x0);
      addClause(mkLit(eqAuxVarID), true);
      addClause(mkLit(p0), true);
    }
  else
    {
      if(f_x0 > 0)
        addClause(~mkLit(x0-1), mkLit(f_x0 - 1), true);
      else
        addClause(~mkLit(x0-1), ~mkLit(abs(f_x0) - 1), true);
      addClause(mkLit(p0), true);
    }
  return p0;
}

int Solver::addShatterSBP(unsigned int prevX, int f_prevX, unsigned int currentX, int f_currentX, int currentP){
  this->newSymmAuxVar();
  int nextP = this->nVars() - 1;
  if(symm_eq_aux){
    unsigned int prevEqAuxVarID = this->addEqAuxVars(prevX, f_prevX);
    unsigned int currentEqAuxVarID = this->addEqAuxVars(currentX, f_currentX);
    //printf("debug: %d %d\n", currentP, prevEqAuxVarID + 1);
    addClause(~mkLit(currentP), ~mkLit(prevEqAuxVarID + 1), mkLit(currentEqAuxVarID), true);
    addClause(~mkLit(currentP), ~mkLit(prevEqAuxVarID + 1), mkLit(nextP), true);            
  }
  else{
    if(f_currentX > 0)
      addClause(~mkLit(currentP), ~mkLit((int)prevX-1), ~mkLit((int)(currentX-1)), mkLit(f_currentX - 1), true);
    else
      addClause(~mkLit(currentP), ~mkLit((int)prevX-1), ~mkLit((int)(currentX-1)), ~mkLit(abs(f_currentX) - 1), true);
    addClause(~mkLit(currentP), ~mkLit((int)prevX-1), mkLit(nextP), true);            
    vec<Lit> clause3;
    clause3.clear();
    clause3.push(~mkLit(currentP));
    if(f_prevX > 0)
      clause3.push( mkLit(f_prevX - 1));
    else
      clause3.push(~mkLit(abs(f_prevX) - 1));
    clause3.push(~mkLit((int)currentX - 1));
    if(f_currentX > 0)
      clause3.push( mkLit(f_currentX - 1));
    else
      clause3.push(~mkLit(abs(f_currentX) - 1));
    this->addClause_(clause3, true);
    if(f_currentX > 0)
      addClause(~mkLit(currentP), mkLit(f_prevX - 1), mkLit(nextP), true);
    else
      addClause(~mkLit(currentP), ~mkLit(abs(f_prevX) - 1), mkLit(nextP), true);
  }
  return nextP;
}


void Solver::addAllShatterSBPs(int* perm, unsigned int* support, unsigned int nsupport)
  {
    int p0 = this->addInitShatterSBP(support[0], perm[support[0]]);
    //printf("Adding shatter SBP clauses\n");
    unsigned int i = 0 ;
    currentP = p0;
    for (i = 1; i < nsupport; ++i)
      {
        int nextP = this->addShatterSBP(support[i-1], perm[support[i-1]], support[i], perm[support[i]], currentP);
        currentP = nextP ;
      }
    //printf("Added shatter SBP clauses\n");
  }

int Solver::addInitChainingSBP(unsigned int x0, int f_x0)
  {
    this->newSymmAuxVar();
    int p0 = this->nVars() - 1;
    if(symm_eq_aux)
      {
        unsigned int eps1 = this->addEqAuxVars(x0, f_x0);
        addClause(mkLit(eps1), true);
        addClause(~mkLit(eps1 + 1), mkLit(p0), true);
      }
    else
      {
        if(f_x0 > 0)
          addClause(~mkLit(x0-1), mkLit(f_x0 - 1), true);
        else
          addClause(~mkLit(x0-1), ~mkLit(abs(f_x0) - 1), true);
        addClause(~mkLit(x0-1), mkLit(p0), true);
        if(f_x0 > 0)
          addClause(mkLit(f_x0 - 1), mkLit(p0), true);
        else
          addClause(~mkLit(abs(f_x0) - 1), mkLit(p0), true);
      }
    return p0;
  }

int Solver::addChainingSBP(unsigned int x, int f_x, int currentP)
  {
    //TODO: stop on phase shifts
    this->newSymmAuxVar();
    int nextP = this->nVars() - 1;
    if(symm_eq_aux)
      {
        unsigned int eqAuxVarID = this->addEqAuxVars(x, f_x);
        addClause(~mkLit(currentP), mkLit(eqAuxVarID), true);
        addClause(~mkLit(currentP), ~mkLit(eqAuxVarID + 1), mkLit(nextP), true);
      }
    //printf("ThisVar = %d NextVar = %d\r\n", currentP, nextP);
    else
      {
        if(f_x > 0)
          addClause(~mkLit(currentP), ~mkLit(x-1), mkLit(f_x - 1), true);
        else
          addClause(~mkLit(currentP), ~mkLit(x-1), ~mkLit(abs(f_x) - 1), true);
        if(f_x > 0)
          addClause(~mkLit(currentP), mkLit(f_x - 1), mkLit(nextP), true);
        else
          addClause(~mkLit(currentP), ~mkLit(abs(f_x) - 1), mkLit(nextP), true);
        addClause(~mkLit(currentP), ~mkLit(x-1), mkLit(nextP), true);
      }
    return nextP;
  }


void Solver::addAllChainingSBPs(int* perm, unsigned int* support, unsigned int nsupport)
  {
    //printf("Adding chaining SBP clauses\n");
    unsigned int i = 0 ;
    int p0 = addInitChainingSBP(support[0], perm[support[0]]);
    currentP = p0;
    for (i = 1; i < nsupport; ++i)
      {
        currentP = addChainingSBP(support[i], perm[support[i]], currentP);
      }
    //printf("Added chaining SBP clauses\n");
  }


void Solver::initVarEqs()
  {
    printf("Solver nVars() = %d\n",this->nVars());
    this->eqs = (PARRAY*) malloc((this->nVars() + 1) * sizeof(int*) );
    int i = 0 ;
    for( i = 0 ; i <= this->nVars() ; i ++)
      {
        this->eqs[i] = paMake(5,5);
        if (this->eqs[i] == NULL)
          {
            printf("Could not init var_eqs, exiting!!\r\n");
            exit(-1);
          }
      }
  }

void Solver::cleanVarEqs()
  {
    int i = 0 ;
    for( i = 0 ; i <= this->nVars() ; i ++)
      {
        paClose(this->eqs[i]);
      }
    free(this->eqs);
  }

void Solver::addEq(long int l1, long int l2)
  {
    //printf("Adding equality\n");
    NumNaiveEqs++;    
    Minisat::Eq* newEq = (Minisat::Eq*)malloc(sizeof(Eq));
    newEq -> added = 0;
    newEq -> defAdded = 0;
    if(symm_dynamic){
      newEq -> succ = malloc(sizeof(Minisat::Eq*) * this->nSymmetries);
      unsigned int i = 0;
      for(i = 0 ; i < this-> nSymmetries ; i++)
	((Minisat::Eq**)(newEq -> succ)) [i]= NULL; }
    else
      newEq -> succ = NULL;
    if(symm_dynamic){
      newEq -> pred = malloc(sizeof(Minisat::Eq*) * this->nSymmetries);
      unsigned int i = 0;
      for(i = 0 ; i < this-> nSymmetries ; i++)
        ((Minisat::Eq**) (newEq -> pred))[i] = NULL; }
    else
      newEq -> pred = NULL;
    if(l1 < 0 && l2 > 0)
      {
        newEq -> v = l2;
        newEq -> l = l1;
        // printf("Added eqs: %d\n", NumEqs);
        // printf("Adding %d -> %d\n", l1, l2);
        // printf("Size of eqs = %d\n", paSize(this->eqs[newEq -> v]));
        int eq_idx = paContains(this->eqs[newEq -> v], eqCmp, (void*) newEq);
        if(eq_idx >= 0)
          {
            free(newEq);
            return;
          }
        paAdd(this->eqs[-l1], (void*) newEq);
        paAdd(this->eqs[l2], (void*) newEq);
      }
    else if(l1 > 0 && l2 < 0)
      {
        newEq -> v = l1;
        newEq -> l = l2;
        // printf("Added eqs: %d\n", NumEqs);
        // printf("Adding %d -> %d\n", l1, l2);
        // printf("Size of eqs = %d\n", paSize(this->eqs[newEq -> v]));
        int eq_idx = paContains(this->eqs[newEq -> v], eqCmp, (void*) newEq);
        if(eq_idx >= 0)
          {
            free(newEq);
            return;
          }
        paAdd(this->eqs[l1], (void*) newEq);
        paAdd(this->eqs[-l2], (void*) newEq);
      }
    else
      {
        newEq -> v = abs(l1);
        newEq -> l = abs(l2);
        // printf("Added eqs: %d\n", NumEqs);
        // printf("Adding %ld -> %ld\n", newEq -> v, newEq -> l);
        // printf("Size of eqs = %d\n", paSize(this->eqs[newEq -> v]));
        int eq_idx = paContains(this->eqs[newEq -> v], eqCmp, (void*) newEq);
        if(eq_idx >= 0)
          {
            free(newEq);
            return;
          }
        paAdd(this->eqs[abs(l1)], newEq);
        paAdd(this->eqs[abs(l2)], newEq);
      }
    NumEqs++;
    /* printf("Adding %ld %ld\n", l1, l2); */
    /* printf("eq_idx1 = %d eq_idx2 = %d\n", eq_idx1, eq_idx2); */
  }

bool Solver::constructEqTable(int* perm, unsigned int* support, unsigned int nsupport)
 {
   unsigned int i = 0;
   for( i = 0 ; i < nsupport ; i++)
     this->addEq(support[i], perm[support[i]]);   
 }

// Add the definitions of the auxiliary variables representing the maping v->l to the formula and return the IDs of the corresponding cnf vars
unsigned int Solver::addEqAuxVars(unsigned int v, int l)
  {
    Minisat::Eq* tempEq = (Minisat::Eq*)malloc(sizeof(Eq));
    tempEq -> added = 0;
    tempEq -> defAdded = 0;
    tempEq -> succ = NULL;
    tempEq -> pred = NULL;
    tempEq -> v = v;
    tempEq -> l = l;
    int eq_idx = paContains(this->eqs[v], eqCmp, (void*) tempEq);
    if (eq_idx < 0)
      {
        printf("Problem with eq table, exiting!!\n");
        exit(-1);
      }
    free(tempEq);
    tempEq = (Minisat::Eq*)paElementAt(this->eqs[v], eq_idx);
    // Adding the definitions of the aux var if they are not there already
    if(!tempEq->defAdded)
      {
        //printf("Adding eq aux vars and their defining clauses\n");
        this->newSymmAuxVar();
        tempEq->cnfVarID = this->nVars() - 1;
        //Var1 def clause
        if(l > 0)
          addClause(~mkLit(tempEq->cnfVarID), ~mkLit(v - 1), mkLit(l - 1), true);
        else
          addClause(~mkLit(tempEq->cnfVarID), ~mkLit(v - 1), ~mkLit(abs(l) - 1), true);

        this->newSymmAuxVar();

        //Var2 def clause 1
        if(l > 0)
          addClause(mkLit(l - 1), mkLit(tempEq->cnfVarID + 1), true);
        else
          addClause(~mkLit(abs(l) - 1), mkLit(tempEq->cnfVarID + 1), true);
 
        //Var2 def clause 2
        addClause(~mkLit(v - 1), mkLit(tempEq->cnfVarID + 1), true);
        tempEq->defAdded = 1;
      }
    return (tempEq->cnfVarID);
  }

// This initialises which equality does every variable watch for every permutation.
void Solver::initEqWatchStructure(int* perm, unsigned int* support, unsigned int nsupport, unsigned int permIdx)
  {
    //printf("Adding chaining SBP clauses\n");
    unsigned int i = 0 ;
    Minisat::Eq* prevEq = NULL;
    prevEq = (Minisat::Eq*)malloc(sizeof(Eq));
    prevEq -> added = 0;
    prevEq -> defAdded = 0;
    prevEq -> succ = NULL;
    prevEq -> pred = NULL;
    prevEq -> v = support[i];
    prevEq -> l = perm[support[i]];
    int eq_idx = paContains(this->eqs[support[i]], eqCmp, (void*) prevEq);
    if (eq_idx < 0)
      {
        printf("Problem with eq table: %d -> %d is not registered, exiting!!\n", prevEq -> v, prevEq -> l);
        exit(-1);
      }
    free(prevEq);
    prevEq = (Minisat::Eq*)paElementAt(this->eqs[support[i]], eq_idx);
    this->watchedEqs[prevEq -> v][permIdx] = prevEq;
    this->watchedEqs[abs(prevEq -> l)][permIdx] = prevEq;
    Minisat::Eq* currentEq = NULL;
    for (i = 1; i < nsupport; ++i)
      {
        currentEq = (Minisat::Eq*)malloc(sizeof(Eq));
        currentEq -> added = 0;
        currentEq -> defAdded = 0;
        // currentEq -> succ = NULL;
        // currentEq -> pred = NULL;
        currentEq -> v = support[i];
        currentEq -> l = perm[support[i]];
        int eq_idx = paContains(this->eqs[support[i]], eqCmp, (void*) currentEq);
        if (eq_idx < 0)
          {
            printf("Problem with eq table, exiting!!\n");
            exit(-1);
          }
        free(currentEq);
        currentEq = (Minisat::Eq*)paElementAt(this->eqs[support[i]], eq_idx);
        ((Minisat::Eq**)(prevEq->succ))[permIdx] = currentEq;
        ((Minisat::Eq**)(currentEq->pred))[permIdx] = prevEq;
        prevEq = currentEq;
      }

    //testing whether the watched eqs were initialised correctly
    // for(int i = 1 ; i <= this->orig_vars ; i++){
    //   printf("i = %d ", i);
    //   fflush(stdout);
    //   printf("this->watchedEqs[i][permIdx] = %d\n", this->watchedEqs[i][permIdx]);
    //   if(this->watchedEqs[i][permIdx] != NULL)
    //     printf("%d watches %d->%d\n", i, (int)this->watchedEqs[i][permIdx]->v, (int)this->watchedEqs[i][permIdx]->l);
    // }
    
    //printf("Added chaining SBP clauses\n");
  }

bool Solver::addSymmetryGenerator(Minisat::Permutation& perm, unsigned int permIdx) {
  //printf("permIdx = %d\n", permIdx);
  if(symm_eq_aux || symm_dynamic)
    this->constructEqTable(perm.f, perm.dom, perm.domSize);
  if(symm_dynamic && symm_break_shatter)
    {
      //Initialise eqs preds and succs
      //Add initial SBPs
    }
  else if(symm_break_shatter)
    addAllShatterSBPs(perm.f, perm.dom, perm.domSize);
  else if(symm_dynamic && symm_break_chaining_imp)
    {
      //Initialise eqs preds and succs
      initEqWatchStructure(perm.f, perm.dom, perm.domSize, permIdx);
      //Add initial SBPs    
      this->currentP = addInitChainingSBP(perm.dom[0], perm.f[perm.dom[0]]);
    }
  else if(symm_break_chaining_imp)
    addAllChainingSBPs(perm.f, perm.dom, perm.domSize);
  return ok;
}

bool Solver::predSat(Minisat::Eq* eq, int permIdx)
  {
    if(value(eq->v) == value(eq->l))
      if(((Minisat::Eq**) eq->pred)[permIdx] == NULL)
        return 1;
      else
        return predSat(((Minisat::Eq**)eq->pred)[permIdx], permIdx);
    else
      watchedEqs[eq->v][permIdx] = eq, watchedEqs[abs(eq->l)][permIdx] = eq;
  }

bool Solver::addSucc(Minisat::Eq* eq, int permIdx)
  {
    
    if(!eq->added)
      {
        printf("adding SBP for %d -> %d\n", eq->v, eq->l);
        currentP = addChainingSBP(eq->v, eq->l, currentP);
      }
    eq->added = 1;
  }
