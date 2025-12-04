# OoOcpusim
This project implements a cycle-accurate simulator for an N-wide out-of-order superscalar processor. The design models a simplified CPU microarchitecture with five pipeline stages: Fetch, Dispatch, Schedule, Execute, and State Update. The simulator faithfully reproduces pipeline timing, latch behavior, data dependencies, and functional unit scheduling to match reference execution logs.

Key features include:

• Accurate modeling of out-of-order execution with tag-based instruction tracking
• Centralized reservation station with dependency checking and ready-bit updates
• Support for multiple functional unit types and configurable result buses
• Correct half-cycle behavior for register file writes, FU release, scheduling, and state updates
• Cycle-by-cycle simulation output with IPC, dispatch queue statistics, and total runtime
• Validated against benchmark traces such as gcc, gobmk, hmmer, and mcf
