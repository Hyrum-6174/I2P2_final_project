#pragma once
#include "search_types.hpp"
#include "game_history.hpp"
#include "state.hpp"

struct MtdfParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = true;
    bool use_tt = true;

    static MtdfParams from_map(const ParamMap& m){
        MtdfParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial", true);
        p.use_tt            = param_bool(m, "UseTT", true);
        return p;
    }
};

class MTDF_ID {
public:
    static int qsearch(
        State *state,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        int alpha,
        int beta,
        const MtdfParams& p
    );

    static int eval_memory(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        int alpha,
        int beta,
        Move& root_best_move,
        const MtdfParams& p,
        const std::chrono::high_resolution_clock::time_point& t0
    );

    static int mtdf(
        State *state, 
        int first_guess, 
        int depth, 
        GameHistory& history, 
        SearchContext& ctx, 
        const MtdfParams& p, 
        Move& root_best_move,
        const std::chrono::high_resolution_clock::time_point& t0
    );

    static SearchResult search(
        State *state,
        int max_depth, // Will be overridden by time limits via Iterative Deepening
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};