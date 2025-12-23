# OoOcpusim
This project implements a cycle-accurate simulator for an N-wide out-of-order superscalar processor. The design models a simplified CPU microarchitecture with five pipeline stages: Fetch, Dispatch, Schedule, Execute, and State Update. The simulator reproduces pipeline timing, latch behavior, data dependencies, and functional unit scheduling to match reference execution logs.

Key features include:

- Accurate modeling of out-of-order execution with tag-based instruction tracking
- Centralized reservation station with dependency checking and ready-bit updates
- Support for multiple functional unit types and configurable result buses
- Correct half-cycle behavior for register file writes, FU release, scheduling, and state updates
- Cycle-by-cycle simulation output with IPC, dispatch queue statistics, and total runtime
- Validated against benchmark traces such as gcc, gobmk, hmmer, and mcf

## Benchmark Results

The simulator was tested using the provided 100k instruction traces. Results include IPC, dispatch queue statistics, and total cycle count.

### gcc.100k.trace
Processor stats:  
Total instructions: 100000  
Avg dispatch queue size: 26039.072266  
Maximum dispatch queue size: 51965  
Avg inst fired per cycle: 1.921303  
Avg inst retired per cycle: 1.921303  
Total run time (cycles): 52048  

### gobmk.100k.trace
Processor stats:  
Total instructions: 100000  
Avg dispatch queue size: 27406.449219  
Maximum dispatch queue size: 55374  
Avg inst fired per cycle: 1.828421  
Avg inst retired per cycle: 1.828421  
Total run time (cycles): 54692  

### hmmer.100k.trace
Processor stats:  
Total instructions: 100000  
Avg dispatch queue size: 27129.669922  
Maximum dispatch queue size: 54225  
Avg inst fired per cycle: 1.830396  
Avg inst retired per cycle: 1.830396  
Total run time (cycles): 54633  

### mcf.100k.trace
Processor stats:  
Total instructions: 100000  
Avg dispatch queue size: 26875.132812  
Maximum dispatch queue size: 53688  
Avg inst fired per cycle: 1.850995  
Avg inst retired per cycle: 1.850995  
Total run time (cycles): 54025  
