#pragma once
#include "search_types.hpp"
#include "game_history.hpp"
#include "state.hpp"

struct QParams {
    bool use_kp_eval = true;
    bool use_eval_mobility = true;
    bool report_partial = true;
    bool use_tt = false;
    bool use_qsearch = true; // Option to enable/disable Quiescence Search

    static QParams from_map(const ParamMap& m){
        QParams p;
        p.use_kp_eval       = param_bool(m, "UseKPEval", true);
        p.use_eval_mobility = param_bool(m, "UseEvalMobility", true);
        p.report_partial    = param_bool(m, "ReportPartial", true);
        p.use_tt            = param_bool(m, "UseTT", false);
        p.use_qsearch       = param_bool(m, "UseQSearch", true);
        return p;
    }
};

class QuiescenceSearch {
public:
    static int eval_ctx(
        State *state,
        int depth,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        int alpha,
        int beta,
        const QParams& p
    );

    static int qsearch(
        State *state,
        GameHistory& history,
        int ply,
        SearchContext& ctx,
        int alpha,
        int beta,
        const QParams& p
    );

    static SearchResult search(
        State *state,
        int depth,
        GameHistory& history,
        SearchContext& ctx
    );

    static ParamMap default_params();
    static std::vector<ParamDef> param_defs();
};