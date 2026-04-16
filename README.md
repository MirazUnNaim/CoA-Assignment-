# Cache Controller FSM Simulation

This project implements a Finite State Machine (FSM) based cache controller in C++. It simulates a direct-mapped cache using a write-back policy.

## Features
- FSM-based cache controller
- Direct-mapped cache
- Write-back policy
- Handles cache hits, misses, and dirty evictions
- Cycle-by-cycle simulation output

## FSM States
- IDLE
- COMPARE_TAG
- WRITE_BACK
- ALLOCATE

## Build
g++ -std=c++17 -O2 -o cache_sim FSM_Cache_Controller.cpp

## Run
.\cache_sim

## Output
The program prints the FSM state transitions and request results.  
Output is also stored in `simulation_output.txt`.

## Group Members
- Shafeen Sufian Meead (230041206)
- Miraz Un Naim (230041241)
- Khandaker Musabbir Ashad Dibbo(230041253)
