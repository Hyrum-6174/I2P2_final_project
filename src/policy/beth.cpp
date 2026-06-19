#include <algorithm>
#include <chrono>
#include "beth.hpp"

// ==========================================
// Transposition Table Implementation
// ==========================================

TranspositionTable::TranspositionTable(size_t size_mb) {
    resize(size_mb);
}

void TranspositionTable::resize(size_t size_mb) {
    size_t num_entries = (size_mb * 1024 * 1024) / sizeof(TTEntry);
    size_t pow2_size = 1;
    while (pow2_size <= num_entries) pow2_size <<= 1;
    pow2_size >>= 1; 

    if (pow2_size == 0) pow2_size = 1;

    table.assign(pow2_size, TTEntry{0, 0, 0, TTFlag::EXACT, Move{}, false});
    mask = pow2_size - 1;
}

void TranspositionTable::clear() {
    std::fill(table.begin(), table.end(), TTEntry{0, 0, 0, TTFlag::EXACT, Move{}, false});
}

void TranspositionTable::store(uint64_t key, int depth, int value, TTFlag flag, const Move& best_move, bool has_move) {
    TTEntry& entry = table[key & mask];
    if (entry.key != key || depth >= entry.depth) {
        entry.key = key;
        entry.depth = depth;
        entry.value = value;
        entry.flag = flag;
        entry.best_move = best_move;
        entry.has_best_move = has_move;
    }
}

bool TranspositionTable::probe(uint64_t key, int depth, int alpha, int beta, int& out_value, Move& out_best_move, bool& out_has_move) {
    TTEntry& entry = table[key & mask];
    if (entry.key == key) {
        out_best_move = entry.best_move;
        out_has_move = entry.has_best_move;
        
        if (entry.depth >= depth) {
            if (entry.flag == TTFlag::EXACT) {
                out_value = entry.value;
                return true;
            }
            if (entry.flag == TTFlag::LOWERBOUND && entry.value >= beta) {
                out_value = entry.value;
                return true;
            }
            if (entry.flag == TTFlag::UPPERBOUND && entry.value <= alpha) {
                out_value = entry.value;
                return true;
            }
        }
    }
    return false;
}

bool TranspositionTable::probe_raw(uint64_t key, TTEntry& out_entry) {
    TTEntry& entry = table[key & mask];
    if (entry.key == key) {
        out_entry = entry;
        return true;
    }
    return false;
}

// ==========================================
// BethSearch Engine Implementation
// ==========================================

SearchResult BethSearch::search(
    State *state,
    int max_depth,
    GameHistory& history,
    SearchContext& ctx
) {
    ctx.reset();
    BethParams p = BethParams::from_map(ctx.params);
    SearchResult result;
    
    // Shared Transposition Table (Persists across search calls)
    static TranspositionTable tt(p.tt_size_mb);
    
    // Ensure legal actions are generated
    if(state->legal_actions.empty()) state->get_legal_actions();
    
    Move best_move_overall;
    if(!state->legal_actions.empty()) best_move_overall = state->legal_actions[0];

    auto t0 = std::chrono::high_resolution_clock::now();
    int first_guess = 0;

    // Iterative Deepening Setup
    for (int depth = 1; depth <= max_depth; ++depth) {
        int score;

        if (p.use_mtdf) {
            score = mtdf(state, first_guess, depth, history, ctx, p, tt);
        } else {
            // Fallback to standard Alpha-Beta framework window if MTDF is disabled
            score = alpha_beta_memory(state, depth, history, 1, ctx, -P_MAX, P_MAX, p, tt);
        }

        if (ctx.stop) break;

        first_guess = score;
        result.score = score;
        result.depth = depth;

        // Retrieve the definitive best root move mapped by TT 
        TTEntry root_entry;
        if (p.use_tt && tt.probe_raw(state->hash(), root_entry) && root_entry.has_best_move) {
            best_move_overall = root_entry.best_move;
        }
        result.best_move = best_move_overall;

        if (p.report_partial && ctx.on_root_update) {
            ctx.on_root_update({result.best_move, result.score, depth, 1, 1});
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    return result;
}

int BethSearch::mtdf(
    State* state, 
    int first_guess, 
    int depth, 
    GameHistory& history, 
    SearchContext& ctx, 
    const BethParams& p,
    TranspositionTable& tt
) {
    int g = first_guess;
    int upperbound = P_MAX;
    int lowerbound = -P_MAX;

    while (lowerbound < upperbound) {
        if (ctx.stop) break;
        
        // Zero-window search around the current guess
        int beta = std::max(g, lowerbound + 1);
        
        g = alpha_beta_memory(state, depth, history, 1, ctx, beta - 1, beta, p, tt);

        if (g < beta) {
            upperbound = g; // Fail low
        } else {
            lowerbound = g; // Fail high
        }
    }
    return g;
}

int BethSearch::alpha_beta_memory(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const BethParams& p,
    TranspositionTable& tt
){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    // Terminal Node Checks
    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }
    if(state->game_state == WIN) return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    // --- 1. TT Lookup ---
    int tt_val;
    Move tt_move;
    bool has_tt_move = false;
    uint64_t hash = state->hash();

    if (p.use_tt && tt.probe(hash, depth, alpha, beta, tt_val, tt_move, has_tt_move)) {
        return tt_val; 
    }

    // --- 2. Horizon / Quiescence ---
    if(depth <= 0){
        int eval = p.use_qsearch ? 
                   quiescence_search(state, history, ply, ctx, alpha, beta, p) :
                   state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        
        if (p.use_tt) {
            tt.store(hash, depth, eval, TTFlag::EXACT, Move{}, false);
        }
        return eval;
    }

    history.push(hash);

    static thread_local std::vector<Move> killers(256);
    if((int)killers.size() <= ply) killers.resize(ply + 1);

    // --- 3. Move Ordering ---
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(state->legal_actions.size());

    for(auto &action : state->legal_actions){
        State* next = state->next_state(action);
        int quick = next->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        bool same = next->same_player_as_parent();
        
        int qscore = same ? quick : -quick;
        qscore += state->move_order_score(action, next);
        
        // Massive priority if TT suggested this move
        if(has_tt_move && tt_move == action) qscore += 200000;
        else if(killers[ply] == action) qscore += 100000;
        
        scored.emplace_back(qscore, action);
        delete next;
    }

    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b){
        return a.first > b.first;
    });

    int best_score = -P_MAX;
    int original_alpha = alpha;
    Move best_move;
    bool found_move = false;

    // --- 4. Recursive Expansion ---
    for(auto &ps : scored){
        const Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(same){
            // Consecutive turn: Maintain perspective
            score = alpha_beta_memory(next, depth - 1, history, ply + 1, ctx, alpha, beta, p, tt);
        } else {
            // Alternating turn: Negamax flip
            int raw = alpha_beta_memory(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, p, tt);
            score = -raw;
        }

        delete next;

        if(score > best_score) {
            best_score = score;
            best_move = action;
            found_move = true;
        }
        
        if(score > alpha) alpha = score;
        if(alpha >= beta){
            killers[ply] = action; // Record killer move
            break; // Beta Cutoff
        }
    }

    history.pop(hash);

    // --- 5. TT Store ---
    if (p.use_tt) {
        TTFlag flag;
        if (best_score <= original_alpha) flag = TTFlag::UPPERBOUND; // Failed low
        else if (best_score >= beta) flag = TTFlag::LOWERBOUND;      // Failed high
        else flag = TTFlag::EXACT;                                   // Exact score within window

        tt.store(hash, depth, best_score, flag, best_move, found_move);
    }

    return best_score;
}

int BethSearch::quiescence_search(
    State *state,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const BethParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN) state->get_legal_actions();
    if(state->game_state == WIN) return P_MAX - ply;
    if(state->game_state == DRAW) return 0;

    int rep_score;
    if(state->check_repetition(history, rep_score)) return rep_score;

    // Stand-Pat evaluation
    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta) return beta;
    if(stand_pat > alpha) alpha = stand_pat;

    history.push(state->hash());

    // Generate Tactical Moves Only
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(state->legal_actions.size());

    int opp = 1 - state->player;
    for(auto &action : state->legal_actions){
        int target_r = action.second.first;
        int target_c = action.second.second;

        // Is it a capture or promotion?
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
            score = quiescence_search(next, history, ply + 1, ctx, alpha, beta, p);
        } else {
            score = -quiescence_search(next, history, ply + 1, ctx, -beta, -alpha, p);
        }

        delete next;

        if(score > best_score) best_score = score;
        if(score > alpha) alpha = score;
        if(alpha >= beta) break; 
    }

    history.pop(state->hash());
    return best_score;
}

ParamMap BethSearch::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"UseTT", "true"},
        {"UseQSearch", "true"},
        {"UseMTDF", "true"},
        {"TTSizeMB", "16"}
    };
}

std::vector<ParamDef> BethSearch::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "true"},
        {"UseQSearch", ParamDef::CHECK, "true"},
        {"UseMTDF", ParamDef::CHECK, "true"},
        {"TTSizeMB", ParamDef::SPIN, "16"}
    };
}