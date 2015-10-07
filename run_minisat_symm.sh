#!/bin/bash 
build/release/bin/minisat -no-pre  -symm-dynamic -symm-chain \
-symm-aux-freeze -symm=<( { /media/Data/ResearchVisits/SRI2015/sri2015symmetry/saucy-3.0/saucy -c $1 | sed 's/)(/ 0 /g' | sed 's/)/ 0 0/g' | sed 's/(//g' | wc -l ; } ; { /media/Data/ResearchVisits/SRI2015/sri2015symmetry/saucy-3.0/saucy -c $1 | sed 's/)(/ 0 /g' | sed 's/)/ 0 0/g' | sed 's/(//g' ; } ) $1
