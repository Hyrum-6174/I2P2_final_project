#pragma once

#include "search_types.hpp"
#include "game_history.hpp"
#include "state.hpp"
#include <vector>
#include <cstdint>

// --- Parameters ---
struct BethParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = true;
    bool use_tt = true;
    bool use_qsearch = true;
    bool use_mtdf = true;
    int tt_size_mb = 16;

    static BethParams from_map(const ParamMap& m){
        BethParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial", true);
        p.use_tt            = param_bool(m, "UseTT", true);
        p.use_qsearch       = param_bool(m, "UseQSearch", true);
        p.use_mtdf          = param_bool(m, "UseMTDF", true);
        p.tt_size_mb        = param_int(m, "TTSizeMB", 16);
        return p;
    }
};

// --- Transposition Table ---
enum class TTFlag { EXACT, LOWERBOUND, UPPERBOUND };

struct TTEntry {
    uint64_t key;
    int depth;
    int value;
    TTFlag flag;
    Move best_move;
    bool has_best_move;
};

class TranspositionTable {
public:
    TranspositionTable(size_t size_mb);
    void store(uint64_t key, int depth, int value, TTFlag flag, const Move& best_move, bool has_move);
    bool probe(uint64_t key, int depth, int alpha, int beta, int& out_value, Move& out_best_move, bool& out_has_move);
    bool probe_raw(uint64_t key, TTEntry& out_entry); // Useful for root best-move extraction
    void clear();
    void resize(size_t size_mb);

private:
    std::vector<TTEntry> table;
    size_t mask;
};

// --- Search Engine ---
class BethSearch {
public:
    static SearchResult search(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();

private:
    // Core search algorithms
    static int mtdf(
        State* state, 
        int first_guess, 
        int depth, 
        GameHistory& history, 
        SearchContext& ctx, 
        const BethParams& p,
        TranspositionTable& tt
    );

    static int alpha_beta_memory(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        int alpha,
        int beta,
        const BethParams& p,
        TranspositionTable& tt
    );

    static int quiescence_search(
        State *state,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        int alpha,
        int beta,
        const BethParams& p
    );
};