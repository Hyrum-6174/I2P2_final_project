#include <algorithm>
#include <vector>
#include <utility>
#include <chrono>
#include "mtdf_id.hpp"
#include "state.hpp"

// --- Transposition Table Setup ---
enum TTFlag : uint8_t { TT_EXACT = 0, TT_LOWERBOUND = 1, TT_UPPERBOUND = 2 };

struct TTEntry {
    uint64_t hash = 0;
    Move best_move = {{-1, -1}, {-1, -1}};
    int score = 0;
    int depth = -1;
    TTFlag flag = TT_EXACT;
};

// 1 << 23 entries = ~8.3 million entries (approx 200MB, safely inside 4GB limit)
const size_t TT_SIZE = 1 << 23;
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
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN) return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    int rep_score;
    if(state->check_repetition(history, rep_score)) return rep_score;

    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta) return beta;
    if(stand_pat > alpha) alpha = stand_pat;

    history.push(state->hash());

    std::vector<std::pair<int, Move>> scored;
    scored.reserve(state->legal_actions.size());

    int opp = 1 - state->player;
    for(auto &action : state->legal_actions){
        int target_r = action.second.first;
        int target_c = action.second.second;

        int captured = state->piece_at(opp, target_r, target_c);
        int moving_piece = state->piece_at(state->player, action.first.first, action.first.second);
        bool is_promotion = (moving_piece == 1 && (target_r == 0 || target_r == state->board_h() - 1));

        if(captured == 0 && !is_promotion) continue; 

        State* next = state->next_state(action);
        int quick = next->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        bool same = next->same_player_as_parent();
        int qscore = same ? quick : -quick;
        qscore += state->move_order_score(action, next);
        
        scored.emplace_back(qscore, action);
        delete next;
    }

    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b){ return a.first > b.first; });

    int best_score = stand_pat;

    for(auto &ps : scored){
        const Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(same) {
            score = qsearch(next, history, ply + 1, ctx, alpha, beta, p);
        } else {
            score = -qsearch(next, history, ply + 1, ctx, -beta, -alpha, p);
        }

        delete next;

        if(score > best_score) best_score = score;
        if(score > alpha) alpha = score;
        if(alpha >= beta) break; 
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
    // 1. Time Check: Guard against timeout inside deep loops
    if ((ctx.nodes & 2047) == 0) { // Every ~2000 nodes
        auto t_now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration<double, std::milli>(t_now - t0).count() > 9500) {
            ctx.stop = true; 
        }
    }

    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    int original_alpha = alpha;
    uint64_t hash = state->hash();
    TTEntry& tte = TT[hash % TT_SIZE];
    Move tt_move = {{-1, -1}, {-1, -1}};

    // 2. Transposition Table Lookup
    if (p.use_tt && tte.hash == hash) {
        tt_move = tte.best_move;
        if (tte.depth >= depth) {
            if (tte.flag == TT_EXACT) return tte.score;
            if (tte.flag == TT_UPPERBOUND && tte.score <= alpha) return alpha;
            if (tte.flag == TT_LOWERBOUND && tte.score >= beta) return beta;
        }
    }

    // 3. Base Cases
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }
    if(state->game_state == WIN) return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    int rep_score;
    if(state->check_repetition(history, rep_score)) return rep_score;

    if(depth <= 0){
        return qsearch(state, history, ply, ctx, alpha, beta, p);
    }

    history.push(hash);

    static thread_local std::vector<Move> killers(256);
    if((int)killers.size() <= ply) killers.resize(ply + 1);

    // 4. Move Ordering
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(state->legal_actions.size());
    for(auto &action : state->legal_actions){
        int qscore = 0;
        if (action == tt_move) {
            qscore = 2000000; // Best prior move tested first
        } else if (action == killers[ply]) {
            qscore = 1000000; // Killers next
        } else {
            State* next = state->next_state(action);
            int quick = next->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
            bool same = next->same_player_as_parent();
            qscore = same ? quick : -quick;
            qscore += state->move_order_score(action, next);
            delete next;
        }
        scored.emplace_back(qscore, action);
    }

    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b){ return a.first > b.first; });

    int best_score = M_MAX; // Use proper lower limit
    Move best_move_found = {{-1, -1}, {-1, -1}};

    for(auto &ps : scored){
        const Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(same) {
            score = eval_memory(next, depth - 1, history, ply + 1, ctx, alpha, beta, root_best_move, p, t0);
        } else {
            score = -eval_memory(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, root_best_move, p, t0);
        }
        delete next;

        if (ctx.stop) { history.pop(hash); return 0; }

        if(score > best_score) {
            best_score = score;
            best_move_found = action;
            if (ply == 0) root_best_move = action; // Capture real root move
        }
        if(best_score > alpha) alpha = best_score;
        if(alpha >= beta){
            killers[ply] = action;
            break; 
        }
    }

    history.pop(hash);

    // 5. Store in TT
    if (p.use_tt && !ctx.stop) {
        tte.hash = hash;
        tte.depth = depth;
        tte.score = best_score;
        tte.best_move = best_move_found;
        if (best_score <= original_alpha) tte.flag = TT_UPPERBOUND;
        else if (best_score >= beta) tte.flag = TT_LOWERBOUND;
        else tte.flag = TT_EXACT;
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

    // Time-bound ID loop. We ignore 'max_depth' parameter and run purely on time constraints if needed
    // However, we respect max_depth if it was passed explicitly by a GUI/Engine testing protocol.
    int limit_depth = (max_depth > 0 && max_depth < 100) ? max_depth : 100;

    for (int d = 1; d <= limit_depth; d++) {
        if (ctx.stop) break;

        Move depth_best_move = current_best_move;
        int score = mtdf(state, first_guess, d, history, ctx, p, depth_best_move, t0);

        if (ctx.stop) break; // Time ran out during this depth, discard polluted result

        // Commit successful depth results
        first_guess = score;
        current_best_move = depth_best_move;
        result.best_move = current_best_move;
        result.score = score;
        result.depth = d;

        // Immediately report to game runner
        if(p.report_partial && ctx.on_root_update){
            ctx.on_root_update({result.best_move, score, d, 1, 1});
        }

        // Check overall clock after depth completion
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