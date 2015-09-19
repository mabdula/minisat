/****************************************************************************************[Dimacs.h]
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

#ifndef Minisat_Symm_h
#define Minisat_Symm_h

#include <stdio.h>

#include "minisat/utils/ParseUtils.h"
#include "minisat/core/SolverTypes.h"

namespace Minisat {

  typedef struct{int* f; //Permutaiton, always of size S.numVars()
                 unsigned int* dom; //The variables that are not mapped to themselves
                 unsigned int domSize;//The number of variables that are not mapped to themselves
                 } Permutation;

  typedef struct{int l1, l2; // The two literals involved in the equality
                 void* Succ; // The different equalities that succeed the current one in different permuations
                 void* Pred; // The different equalities that precede the current one in different permuations
                 } Eq;

//=================================================================================================
// SYMMETRY Parser:

template<class B>
static void readCycle(B& in, vec<Lit>& cycle) {
    int     parsed_lit, var;
    cycle.clear();
    for (;;){
        parsed_lit = parseInt(in);
        if (parsed_lit == 0) break;
        var = abs(parsed_lit)-1;
        cycle.push( (parsed_lit > 0) ? mkLit(var) : ~mkLit(var) );
    }
}

template<class B>
static void readGenerator(B& in, vec< vec<Lit> >& generator) {
    for (;;){
      generator.push();
      readCycle(in, generator.last());
      if (generator.last().size() == 0) {
        generator.pop();
        break;
      }
    }
}


template<class B>
  static void readGenerator(B& symmFile, Permutation* perm) {
    int* currentPerm = perm->f;
    unsigned int* support = perm->dom;
    unsigned int* nsupport = &(perm->domSize);

    int l1, l2;
    (*nsupport) = 0;
    while(1)
      {
        l1 = parseInt(symmFile);
        if(l1 == 0) break;
        l2 = parseInt(symmFile);
        //Skipping zero
        parseInt(symmFile); 
        if(abs(l1) <= abs(l2))
          {
            if(l1 > 0)
              {
                //printf("%d->%d\n", l1, l2);
                currentPerm[l1] = l2;
                support[(*nsupport)] = l1;
                (*nsupport)++;
              }
	  }
        else
          {
            if(l2 > 0)
              {
                //printf("%d->%d\n", l2, l1);
                currentPerm[l2] = l1;
                support[(*nsupport)] = l2;
                (*nsupport)++;
              }
          }
      }
}

template<class B, class Solver>
static void parse_SYMM_main(B& in, Solver& S) {
    //printf("Starting to parse perm\r\n");
    /* Permutation* perm2 = new Permutation; */
    Permutation perm;
    perm.f = (int*)malloc(sizeof(int) * (S.nVars() + 1));
    perm.domSize = 0;
    perm.dom = (unsigned int*)malloc(sizeof(int) * (S.nVars() + 1));
    int cnt     = 0;
    for (;;){
        skipWhitespace(in);
        if (*in == EOF) break;
        else if (*in == 'c'){
            skipLine(in);
        } else{
            cnt++;
            //printf("Parsing perm\r\n");
            readGenerator(in, &perm);
            S.addSymmetryGenerator(perm); }
    }
}

// Inserts problem into solver.
//
template<class Solver>
static void parse_SYMM(gzFile input_stream, Solver& S) {
    StreamBuffer in(input_stream);
    parse_SYMM_main(in, S); }

//=================================================================================================
}

#endif
