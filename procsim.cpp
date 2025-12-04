#include "procsim.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {

// ---- constants / helpers ----
constexpr int kMaxRegs = 128;
constexpr int kTypes = 3;
constexpr int kLatency[kTypes] = {1, 1, 1};

int fu_type_from_opcode(int op) {
    // Spec: -1 maps to FU type 1
    if (op < 0) return 1;
    if (op >= kTypes) return op % kTypes;
    return op;
}

struct Inst;
struct FU {
    int type = 0;
    Inst* inst = nullptr;
    int remaining = 0;
};

struct Inst {
    proc_inst_t raw{};
    uint64_t tag = 0;

    // stage entry cycles
    uint64_t fetch_c = 0;
    uint64_t disp_c = 0;
    uint64_t sched_c = 0;
    uint64_t exec_c = 0;
    uint64_t state_c = 0;
    uint64_t sched_ready_c = 0;  // cycle when ready to be issued

    // scheduling state
    bool src_ready[2] = {false, false};
    int64_t src_tag[2] = {-1, -1}; // producer tag if waiting

    bool issued = false;
    bool waiting_bus = false;
    bool enqueued_bus = false;
    uint64_t completion_c = 0;

    int type = 0;
    FU* fu = nullptr;
};

// ---- global sim state ----
uint64_t gF = DEFAULT_F;
uint64_t gR = DEFAULT_R;
uint64_t gK[kTypes] = {DEFAULT_K0, DEFAULT_K1, DEFAULT_K2};
uint64_t gRS_cap = 0;

uint64_t g_next_tag = 1;
bool g_trace_done = false;

// storage to keep inst pointers stable
std::vector<std::unique_ptr<Inst>> g_store;

// queues / latches
std::deque<Inst*> q_dispatch;          // dispatch queue
std::vector<Inst*> rs;                // reservation station (combined ROB/RS)
std::vector<Inst*> st_update;         // instructions entering state update this cycle
std::vector<Inst*> bus_wait;          // completed ops waiting for CDB

std::vector<Inst*> latch_fd_cur, latch_fd_nxt; // Fetch->Dispatch latch
std::vector<Inst*> latch_ds_cur, latch_ds_nxt; // Dispatch->Schedule latch
std::vector<Inst*> latch_se_cur, latch_se_nxt; // Schedule->Execute latch

// reg rename/ready table: -1 means ready (no outstanding writer)
std::array<int64_t, kMaxRegs> reg_map;

// FU pool
std::vector<FU> fu_pool;

// stats accumulators
uint64_t g_cycle = 0;
uint64_t g_retired = 0;
uint64_t g_issued_total = 0;
double g_disp_q_sum = 0.0;
uint64_t g_disp_q_max = 0;

// ---- utility ----
bool pipeline_empty() {
    if (!q_dispatch.empty() || !rs.empty() || !st_update.empty() || !bus_wait.empty()) return false;
    if (!latch_fd_cur.empty() || !latch_fd_nxt.empty() ||
        !latch_ds_cur.empty() || !latch_ds_nxt.empty() ||
        !latch_se_cur.empty() || !latch_se_nxt.empty()) return false;
    for (auto& fu : fu_pool) {
        if (fu.inst != nullptr) return false;
    }
    return true;
}

void remove_from_rs(Inst* inst) {
    auto it = std::find(rs.begin(), rs.end(), inst);
    if (it != rs.end()) rs.erase(it);
}

void wakeup_rs_sources(int64_t producer) {
    for (Inst* w : rs) {
        for (int s = 0; s < 2; ++s) {
            if (!w->src_ready[s] && w->src_tag[s] == producer) {
                w->src_ready[s] = true;
                w->src_tag[s] = -1;
            }
        }
    }
}

FU* find_free_fu(int type) {
    for (auto& fu : fu_pool) {
        if (fu.type == type && fu.inst == nullptr) return &fu;
    }
    return nullptr;
}

// Lookahead: count how many FUs of each type will be free at the *start of next cycle's execute*.
std::array<int, kTypes> projected_free_fus(uint64_t cycle) {
    std::array<int, kTypes> free_cnt = {0, 0, 0};

    struct Candidate {
        Inst* inst;
        int type;
        uint64_t free_cycle;
    };
    std::vector<Candidate> candidates;

    for (const auto& fu : fu_pool) {
        if (fu.inst == nullptr) {
            free_cnt[fu.type]++;
            continue;
        }
        Inst* in = fu.inst;
        if (in->waiting_bus) {
            candidates.push_back({in, fu.type, in->completion_c});
        } else if (fu.remaining == 1) {
            candidates.push_back({in, fu.type, cycle + 1});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.free_cycle == b.free_cycle) return a.inst->tag < b.inst->tag;
                  return a.free_cycle < b.free_cycle;
              });

    uint64_t grant = std::min<uint64_t>(gR, candidates.size());
    for (uint64_t i = 0; i < grant; ++i) {
        free_cnt[candidates[i].type]++;
    }
    return free_cnt;
}

// ---- pipeline stage functions ----

// Stage 5: retire (state update lasts 1 cycle; we clear those that entered last cycle)
void retire_state_update() {
    if (st_update.empty()) return;
    for (Inst* inst : st_update) {
        remove_from_rs(inst);
        inst->issued = true; // already done, just to be safe
        g_retired++;
    }
    st_update.clear();
}

// Stage 4a: advance all executing FUs; enqueue completions for bus arbitration
void tick_execute_units(uint64_t cycle) {
    for (auto& fu : fu_pool) {
        if (fu.inst == nullptr) continue;

        if (fu.remaining > 0) {
            fu.remaining--;
            if (fu.remaining == 0 && !fu.inst->waiting_bus) {
                if (fu.inst->completion_c == 0) fu.inst->completion_c = cycle;
                fu.inst->waiting_bus = true;
                if (!fu.inst->enqueued_bus) {
                    bus_wait.push_back(fu.inst);
                    fu.inst->enqueued_bus = true;
                }
            }
        }
    }
}

// Stage 4b: broadcast on CDBs (tag order among oldest completions), free FU only when broadcasted
void broadcast_results(uint64_t cycle) {
    if (bus_wait.empty()) return;

    std::vector<Inst*> ordered = bus_wait;
    std::sort(ordered.begin(), ordered.end(),
              [](Inst* a, Inst* b) {
                  if (a->completion_c == b->completion_c) return a->tag < b->tag;
                  return a->completion_c < b->completion_c;
              });

    std::vector<Inst*> remaining;
    uint64_t used = 0;

    for (Inst* inst : ordered) {
        if (used >= gR) {
            remaining.push_back(inst);
            continue;
        }

        used++;
        inst->waiting_bus = false;
        inst->enqueued_bus = false;

        // free FU now that it broadcasts
        if (inst->fu) {
            inst->fu->inst = nullptr;
            inst->fu->remaining = 0;
            inst->fu = nullptr;
        }

        // clear reg mapping if still youngest writer
        int d = inst->raw.dest_reg;
        if (d >= 0 && d < kMaxRegs) {
            if (reg_map[d] == static_cast<int64_t>(inst->tag)) {
                reg_map[d] = -1;
            }
        }

        // wake dependents
        wakeup_rs_sources(static_cast<int64_t>(inst->tag));

        inst->state_c = cycle;
        st_update.push_back(inst);
    }

    bus_wait.swap(remaining);
}

// Stage 4c: move issued-to-exec latch into actual FUs
void start_executions(uint64_t cycle) {
    if (latch_se_cur.empty()) return;

    for (Inst* inst : latch_se_cur) {
        FU* fu = find_free_fu(inst->type);
        assert(fu && "start_executions saw no available FU (projection bug)");
        fu->inst = inst;
        fu->remaining = kLatency[inst->type];
        inst->fu = fu;
        inst->exec_c = cycle;
    }
    latch_se_cur.clear();
}

// Stage 3a: insert dispatch->schedule latch into RS; set up src readiness and dest rename
void insert_into_rs(uint64_t cycle) {
    if (latch_ds_cur.empty()) return;

    for (Inst* inst : latch_ds_cur) {
        inst->sched_c = cycle;
        inst->sched_ready_c = cycle;  // ready to issue same cycle

        for (int s = 0; s < 2; ++s) {
            int r = inst->raw.src_reg[s];
            if (r < 0 || r >= kMaxRegs) {
                inst->src_ready[s] = true;
                inst->src_tag[s] = -1;
            } else if (reg_map[r] == -1) {
                inst->src_ready[s] = true;
                inst->src_tag[s] = -1;
            } else {
                inst->src_ready[s] = false;
                inst->src_tag[s] = reg_map[r];
            }
        }

        int d = inst->raw.dest_reg;
        if (d >= 0 && d < kMaxRegs) {
            reg_map[d] = static_cast<int64_t>(inst->tag);
        }

        rs.push_back(inst);
    }

    latch_ds_cur.clear();
}

// Stage 2b: move fetch->dispatch latch into dispatch queue
void move_into_dispatch(uint64_t cycle) {
    if (latch_fd_cur.empty()) return;

    for (Inst* inst : latch_fd_cur) {
        inst->disp_c = cycle;
        q_dispatch.push_back(inst);
    }

    latch_fd_cur.clear();
}

// Stage 3b: issue ready RS entries into schedule->exec latch (starts next cycle)
size_t issue_ready(uint64_t cycle) {
    if (rs.empty()) return 0;

    std::vector<Inst*> ordered = rs;
    std::sort(ordered.begin(), ordered.end(),
              [](Inst* a, Inst* b){ return a->tag < b->tag; });

    std::array<int, kTypes> free_next = projected_free_fus(cycle);
    std::array<int, kTypes> reserved = {0, 0, 0};

    size_t fired = 0;
    for (Inst* inst : ordered) {
        if (inst->issued) continue;
        if (cycle < inst->sched_ready_c) continue;  // not ready yet
        if (!(inst->src_ready[0] && inst->src_ready[1])) continue;

        int t = inst->type;
        if (free_next[t] - reserved[t] <= 0) continue;

        inst->issued = true;
        reserved[t]++;
        fired++;
        latch_se_nxt.push_back(inst);
    }
    return fired;
}

// Stage 2a: move from dispatch queue to dispatch->schedule latch, in program order
void dispatch_to_schedule(uint64_t /*cycle*/) {
    while (!q_dispatch.empty()) {
        if (rs.size() + latch_ds_nxt.size() >= gRS_cap) break;
        Inst* inst = q_dispatch.front();
        q_dispatch.pop_front();
        latch_ds_nxt.push_back(inst);
    }
}

// Stage 1: fetch up to F instructions into fetch->dispatch latch
void fetch_instructions(uint64_t cycle) {
    if (g_trace_done) return;

    for (uint64_t i = 0; i < gF; ++i) {
        proc_inst_t raw{};
        if (!read_instruction(&raw)) {
            g_trace_done = true;
            break;
        }

        auto inst = std::unique_ptr<Inst>(new Inst());
        inst->raw = raw;
        inst->tag = g_next_tag++;
        inst->type = fu_type_from_opcode(raw.op_code);
        inst->fetch_c = cycle;

        Inst* p = inst.get();
        latch_fd_nxt.push_back(p);
        g_store.push_back(std::move(inst));
    }
}

// end-of-cycle latch advance
void advance_latches() {
    latch_fd_cur = std::move(latch_fd_nxt);
    latch_fd_nxt.clear();

    latch_ds_cur = std::move(latch_ds_nxt);
    latch_ds_nxt.clear();

    latch_se_cur = std::move(latch_se_nxt);
    latch_se_nxt.clear();
}

} // namespace

// ---- required API ----

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f)
{
    gF = f;
    gR = (r == 0) ? 1 : r;
    gK[0] = k0; gK[1] = k1; gK[2] = k2;
    gRS_cap = 2 * (k0 + k1 + k2);

    g_next_tag = 1;
    g_trace_done = false;
    g_store.clear();

    q_dispatch.clear();
    rs.clear();
    st_update.clear();
    bus_wait.clear();

    latch_fd_cur.clear(); latch_fd_nxt.clear();
    latch_ds_cur.clear(); latch_ds_nxt.clear();
    latch_se_cur.clear(); latch_se_nxt.clear();

    reg_map.fill(-1);

    fu_pool.clear();
    for (int t = 0; t < kTypes; ++t) {
        for (uint64_t i = 0; i < gK[t]; ++i) {
            FU fu;
            fu.type = t;
            fu.inst = nullptr;
            fu.remaining = 0;
            fu_pool.push_back(fu);
        }
    }

    g_cycle = 0;
    g_retired = 0;
    g_issued_total = 0;
    g_disp_q_sum = 0.0;
    g_disp_q_max = 0;
}

void run_proc(proc_stats_t* p_stats)
{
    if (!p_stats) return;

    uint64_t cycle = 0;

    while (!g_trace_done || !pipeline_empty()) {
        cycle++;

        // reverse / half-cycle ordering
        retire_state_update();          // stage 5
        tick_execute_units(cycle);      // stage 4a
        broadcast_results(cycle);       // stage 4b
        start_executions(cycle);        // stage 4c
        insert_into_rs(cycle);          // stage 3a
        move_into_dispatch(cycle);      // stage 2b

        // dispatch queue stats observe after latch move
        g_disp_q_sum += static_cast<double>(q_dispatch.size());
        g_disp_q_max = std::max<uint64_t>(g_disp_q_max,
                                          static_cast<uint64_t>(q_dispatch.size()));

        size_t fired = issue_ready(cycle);  // stage 3b
        g_issued_total += fired;

        dispatch_to_schedule(cycle);    // stage 2a
        fetch_instructions(cycle);      // stage 1

        advance_latches();
    }

    if (g_next_tag == 1) {
        p_stats->cycle_count = 0;
        p_stats->retired_instruction = 0;
        return;
    }

    // we exit one cycle after last useful work
    if (cycle > 0) cycle--;

    g_cycle = cycle;
    p_stats->cycle_count = cycle;
    p_stats->retired_instruction = g_retired;
}

void complete_proc(proc_stats_t* p_stats)
{
    if (!p_stats) return;

    if (p_stats->cycle_count == 0) {
        p_stats->avg_inst_fired = 0.0f;
        p_stats->avg_inst_retired = 0.0f;
        p_stats->avg_disp_size = 0.0f;
        p_stats->max_disp_size = 0;
        return;
    }

    p_stats->avg_inst_fired =
        static_cast<float>(g_issued_total) /
        static_cast<float>(p_stats->cycle_count);

    p_stats->avg_inst_retired =
        static_cast<float>(p_stats->retired_instruction) /
        static_cast<float>(p_stats->cycle_count);

    p_stats->avg_disp_size =
        static_cast<float>(g_disp_q_sum /
        static_cast<double>(p_stats->cycle_count));

    p_stats->max_disp_size = g_disp_q_max;
}
