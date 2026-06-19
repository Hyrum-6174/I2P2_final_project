#include <algorithm>
#include <vector>
#include <utility>
#include <chrono>
#include "mtdf_id.hpp"
#include "state.hpp"
#include <limits>

namespace {
    constexpr Move NO_MOVE = {{-1, -1}, {-1, -1}};

    inline bool same_move(const Move& a, const Move& b) {
        return a.first.first  == b.first.first &&
               a.first.second == b.first.second &&
               a.second.first == b.second.first &&
               a.second.second == b.second.second;
    }

    inline int piece_value(int piece) {
        switch (piece) {
            case 1: return 100;
            case 2: return 300;
            case 3: return 500;
            case 4: return 900;
            case 5: return 1000;
            default: return 0;
        }
    }

    inline int quiet_history_index(const State* s, const Move& mv) {
        const int w = s->board_w();
        const int h = s->board_h();
        const int cells = w * h;
        const int from_sq = mv.first.first * w + mv.first.second;
        const int to_sq   = mv.second.first * w + mv.second.second;
        return from_sq * cells + to_sq;
    }

    inline bool is_tactical_move(const State* s, const Move& mv) {
        const int opp = 1 - s->player;
        const int tr = mv.second.first;
        const int tc = mv.second.second;

        const int captured = s->piece_at(opp, tr, tc);
        const int moving   = s->piece_at(s->player, mv.first.first, mv.first.second);
        const bool promo = (moving == 1 && (tr == 0 || tr == s->board_h() - 1));

        return captured != 0 || promo;
    }

    inline int tactical_move_score(const State* s, const Move& mv, const State* next) {
        const int opp = 1 - s->player;
        const int tr = mv.second.first;
        const int tc = mv.second.second;

        const int captured = s->piece_at(opp, tr, tc);
        const int moving   = s->piece_at(s->player, mv.first.first, mv.first.second);

        int score = 0;

        if (captured != 0) {
            score += 100000 + 10 * piece_value(captured) - piece_value(moving);
        }

        if (moving == 1 && (tr == 0 || tr == s->board_h() - 1)) {
            score += 20000;
        }

        if (next->same_player_as_parent()) {
            score += 15000;
        }

        score += s->move_order_score(mv, next);
        return score;
    }

    inline int quiet_move_score(const State* s, const Move& mv, const std::vector<int>& history_table) {
        int score = 0;
        score += history_table[quiet_history_index(s, mv)];
        return score;
    }
}

// --- Transposition Table Setup ---
enum TTFlag : uint8_t { TT_EXACT = 0, TT_LOWERBOUND = 1, TT_UPPERBOUND = 2 };

struct TTEntry {
    uint64_t hash = 0;
    Move best_move = {{-1, -1}, {-1, -1}};
    int score = 0;
    int depth = -1;
    TTFlag flag = TT_EXACT;
};

// Declared sizes up here so TT_MASK compiles successfully
constexpr size_t TT_SIZE = 1 << 23; 
static constexpr size_t TT_MASK = TT_SIZE - 1;
static std::vector<TTEntry> TT;

// --- Quiescence Search (Tactical extension) ---
int MTDF_ID::qsearch(
    State *state,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const MtdfParams& p
){
    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }

    if (state->game_state == WIN)  return P_MAX - ply;
    if (state->game_state == DRAW) return 0;

    int rep_score;
    if (state->check_repetition(history, rep_score)) return rep_score;

    const int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    history.push(state->hash());

    std::vector<std::pair<int, Move>> tactical;
    tactical.reserve(state->legal_actions.size());

    int max_tactical_gain = 0;

    for (const auto& action : state->legal_actions) {
        if (!is_tactical_move(state, action)) continue;

        State* next = state->next_state(action);
        const int score = tactical_move_score(state, action, next);
        tactical.emplace_back(score, action);

        const int opp = 1 - state->player;
        const int tr = action.second.first;
        const int tc = action.second.second;
        const int captured = state->piece_at(opp, tr, tc);
        const int moving = state->piece_at(state->player, action.first.first, action.first.second);

        int gain = 0;
        if (captured != 0) gain += piece_value(captured) - piece_value(moving);
        if (moving == 1 && (tr == 0 || tr == state->board_h() - 1)) gain += 200;
        if (next->same_player_as_parent()) gain += 100;

        if (gain > max_tactical_gain) max_tactical_gain = gain;

        delete next;
    }

    if (tactical.empty()) {
        history.pop(state->hash());
        return stand_pat;
    }

    const int delta_margin = 150;
    if (stand_pat + max_tactical_gain + delta_margin <= alpha) {
        history.pop(state->hash());
        return alpha;
    }

    std::sort(tactical.begin(), tactical.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    int best_score = stand_pat;

    for (const auto& ps : tactical) {
        const Move action = ps.second;
        State* next = state->next_state(action);

        int score;
        if (next->same_player_as_parent()) {
            score = qsearch(next, history, ply + 1, ctx, alpha, beta, p);
        } else {
            score = -qsearch(next, history, ply + 1, ctx, -beta, -alpha, p);
        }

        delete next;

        if (score > best_score) best_score = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
        if (ctx.stop) break;
    }

    history.pop(state->hash());
    return best_score;
}

// --- Alpha Beta with Memory (Zero Window driver) ---
int MTDF_ID::eval_memory(
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
){
    if ((ctx.nodes & 1023) == 0) {
        auto t_now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration<double, std::milli>(t_now - t0).count() > 9500) {
            ctx.stop = true;
            return 0;
        }
    }

    ctx.nodes++;
    if (ply > ctx.seldepth) ctx.seldepth = ply;
    if (ctx.stop) return 0;

    const int original_alpha = alpha;
    const uint64_t hash = state->hash();
    TTEntry& tte = TT[hash & TT_MASK];

    Move tt_move = NO_MOVE;

    if (p.use_tt && tte.hash == hash) {
        tt_move = tte.best_move;
        if (tte.depth >= depth) {
            if (tte.flag == TT_EXACT)       return tte.score;
            if (tte.flag == TT_UPPERBOUND && tte.score <= alpha) return tte.score;
            if (tte.flag == TT_LOWERBOUND && tte.score >= beta)  return tte.score;
        }
    }

    if (state->legal_actions.empty() && state->game_state == UNKNOWN) {
        state->get_legal_actions();
    }
    if (state->game_state == WIN)  return P_MAX - ply;
    if (state->game_state == DRAW) return 0;

    int rep_score;
    if (state->check_repetition(history, rep_score)) return rep_score;

    if (depth <= 0) {
        return qsearch(state, history, ply, ctx, alpha, beta, p);
    }

    history.push(hash);

    static thread_local std::vector<std::array<Move, 2>> killers;
    static thread_local std::vector<int> history_table;
    static thread_local int hist_w = 0;
    static thread_local int hist_h = 0;

    const int w = state->board_w();
    const int h = state->board_h();
    const int cells = w * h;

    if (hist_w != w || hist_h != h) {
        hist_w = w;
        hist_h = h;
        history_table.assign(cells * cells, 0);
        killers.assign(256, std::array<Move, 2>{NO_MOVE, NO_MOVE});
    } else if ((int)killers.size() <= ply) {
        killers.resize(ply + 1, std::array<Move, 2>{NO_MOVE, NO_MOVE});
    }

    const Move root_hint = root_best_move;

    std::vector<Move> ordered;
    ordered.reserve(state->legal_actions.size());

    std::vector<std::pair<int, Move>> tactical;
    tactical.reserve(state->legal_actions.size());

    for (const auto& action : state->legal_actions) {
        if (p.use_tt && same_move(action, tt_move)) {
            ordered.push_back(action);
            continue;
        }

        // Fix: Only apply root hint ordering when ply is exactly 0
        if (ply == 0 && same_move(action, root_hint)) {
            ordered.push_back(action);
            continue;
        }

        if (same_move(action, killers[ply][0]) || same_move(action, killers[ply][1])) {
            ordered.push_back(action);
            continue;
        }

        if (is_tactical_move(state, action)) {
            State* next = state->next_state(action);
            const int score = tactical_move_score(state, action, next);
            tactical.emplace_back(score, action);
            delete next;
        } else {
            const int score = quiet_move_score(state, action, history_table);
            tactical.emplace_back(score, action);
        }
    }

    std::sort(tactical.begin(), tactical.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    for (const auto& x : tactical) ordered.push_back(x.second);

    int best_score = -P_MAX;
    Move best_move_found = NO_MOVE;

    for (const auto& action : ordered) {
        State* next = state->next_state(action);
        const bool same = next->same_player_as_parent();

        int score;
        if (same) {
            score = eval_memory(next, depth - 1, history, ply + 1, ctx, alpha, beta, root_best_move, p, t0);
        } else {
            score = -eval_memory(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, root_best_move, p, t0);
        }

        delete next;

        if (ctx.stop) {
            history.pop(hash);
            return 0;
        }

        if (score > best_score) {
            best_score = score;
            best_move_found = action;
            if (ply == 0) root_best_move = action;
        }

        if (score > alpha) alpha = score;

        if (alpha >= beta) {
            if (!same_move(action, tt_move)) {
                killers[ply][1] = killers[ply][0];
                killers[ply][0] = action;
            }

            if (!is_tactical_move(state, action)) {
                const int idx = quiet_history_index(state, action);
                history_table[idx] += depth * depth;
                if (history_table[idx] > 1000000000) {
                    for (int& v : history_table) v >>= 1;
                }
            }
            break;
        }
    }

    history.pop(hash);

    if (p.use_tt && !ctx.stop) {
        if (tte.hash != hash || depth >= tte.depth) {
            tte.hash = hash;
            tte.depth = depth;
            tte.score = best_score;
            tte.best_move = best_move_found;

            if (best_score <= original_alpha) tte.flag = TT_UPPERBOUND;
            else if (best_score >= beta)     tte.flag = TT_LOWERBOUND;
            else                             tte.flag = TT_EXACT;
        }
    }

    return best_score;
}

// --- MTD(f) Core ---
int MTDF_ID::mtdf(
    State *state, 
    int first_guess, 
    int depth, 
    GameHistory& history, 
    SearchContext& ctx, 
    const MtdfParams& p, 
    Move& root_best_move,
    const std::chrono::high_resolution_clock::time_point& t0
){
    int g = first_guess;
    int upperbound = P_MAX;
    int lowerbound = -P_MAX;

    while (lowerbound < upperbound && !ctx.stop) {
        int beta = (g == lowerbound) ? g + 1 : g;
        g = eval_memory(state, depth, history, 0, ctx, beta - 1, beta, root_best_move, p, t0);
        if (g < beta) upperbound = g;
        else lowerbound = g;
    }
    return g;
}

// --- Iterative Deepening Runner ---
SearchResult MTDF_ID::search(
    State *state,
    int max_depth, 
    GameHistory& history,
    SearchContext& ctx
){
    if (TT.empty()) TT.resize(TT_SIZE);

    ctx.reset();
    MtdfParams p = MtdfParams::from_map(ctx.params);
    SearchResult result;
    result.depth = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    if(!state->legal_actions.size()) state->get_legal_actions();
    if(!state->legal_actions.empty()) result.best_move = state->legal_actions[0];

    int first_guess = 0;
    Move current_best_move = result.best_move;

    int limit_depth = (max_depth > 0 && max_depth < 100) ? max_depth : 100;

    for (int d = 1; d <= limit_depth; d++) {
        if (ctx.stop) break;

        Move depth_best_move = current_best_move;
        int score = mtdf(state, first_guess, d, history, ctx, p, depth_best_move, t0);

        if (ctx.stop) break; 

        first_guess = score;
        current_best_move = depth_best_move;
        result.best_move = current_best_move;
        result.score = score;
        result.depth = d;

        if(p.report_partial && ctx.on_root_update){
            ctx.on_root_update({result.best_move, score, d, 1, 1});
        }

        auto t_now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration<double, std::milli>(t_now - t0).count() > 9500) {
            break; 
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    return result;
}

ParamMap MTDF_ID::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"UseTT", "true"}
    };
}

std::vector<ParamDef> MTDF_ID::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "true"}
    };
}
