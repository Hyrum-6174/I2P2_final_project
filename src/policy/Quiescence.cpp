#include <algorithm>
#include <vector>
#include <utility>
#include <chrono>
#include "quiescence.hpp"
#include "state.hpp"

int QuiescenceSearch::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const QParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    // Base Horizon Case
    if(depth <= 0){
        if (p.use_qsearch) {
            return qsearch(state, history, ply, ctx, alpha, beta, p);
        } else {
            int score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
            return score;
        }
    }

    history.push(state->hash());

    // Killer table: lightweight per-ply last-cutoff move.
    static thread_local std::vector<Move> killers(256);
    if((int)killers.size() <= ply) killers.resize(ply + 1);

    // Move ordering: quick static eval of each child to sort promising moves first.
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(state->legal_actions.size());
    for(auto &action : state->legal_actions){
        State* next = state->next_state(action);
        int quick = next->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        bool same = next->same_player_as_parent();
        int qscore = same ? quick : -quick;
        qscore += state->move_order_score(action, next);
        if(killers[ply] == action){
            qscore += 100000; 
        }
        scored.emplace_back(qscore, action);
        delete next;
    }

    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b){
        return a.first > b.first;
    });

    int best_score = M_MAX;

    for(auto &ps : scored){
        const Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(same){
            score = eval_ctx(next, depth - 1, history, ply + 1, ctx, alpha, beta, p);
        } else {
            int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, p);
            score = -raw;
        }

        delete next;

        if(score > best_score) best_score = score;
        if(score > alpha) alpha = score;
        if(alpha >= beta){
            killers[ply] = action;
            break; 
        }
    }

    history.pop(state->hash());
    return best_score;
}

int QuiescenceSearch::qsearch(
    State *state,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const QParams& p
){
    ctx.nodes++;
    if(ply > ctx.seldepth) ctx.seldepth = ply;
    if(ctx.stop) return 0;

    if(state->legal_actions.empty() && state->game_state == UNKNOWN){
        state->get_legal_actions();
    }

    if(state->game_state == WIN){
        return P_MAX - ply;
    }
    if(state->game_state == DRAW){
        return 0;
    }

    int rep_score;
    if(state->check_repetition(history, rep_score)){
        return rep_score;
    }

    // --- Stand-Pat Bounds Check ---
    int stand_pat = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
    if(stand_pat >= beta){
        return beta;
    }
    if(stand_pat > alpha){
        alpha = stand_pat;
    }

    history.push(state->hash());

    // --- Generate and Filter Tactical Moves Only ---
    std::vector<std::pair<int, Move>> scored;
    scored.reserve(state->legal_actions.size());

    int opp = 1 - state->player;
    for(auto &action : state->legal_actions){
        int target_r = action.second.first;
        int target_c = action.second.second;

        // Check if the destination contains an enemy piece (Capture)
        int captured = state->piece_at(opp, target_r, target_c);
        
        // Check if a pawn reaches the promotion rank (Promotion)
        int moving_piece = state->piece_at(state->player, action.first.first, action.first.second);
        bool is_promotion = (moving_piece == 1 && (target_r == 0 || target_r == state->board_h() - 1));

        // Skip quiet positions entirely during Q-search
        if(captured == 0 && !is_promotion){
            continue; 
        }

        State* next = state->next_state(action);
        int quick = next->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        bool same = next->same_player_as_parent();
        int qscore = same ? quick : -quick;
        qscore += state->move_order_score(action, next);
        
        scored.emplace_back(qscore, action);
        delete next;
    }

    std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b){
        return a.first > b.first;
    });

    int best_score = stand_pat;

    for(auto &ps : scored){
        const Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(same){
            score = qsearch(next, history, ply + 1, ctx, alpha, beta, p);
        } else {
            int raw = qsearch(next, history, ply + 1, ctx, -beta, -alpha, p);
            score = -raw;
        }

        delete next;

        if(score > best_score) best_score = score;
        if(score > alpha) alpha = score;
        if(alpha >= beta){
            break; 
        }
    }

    history.pop(state->hash());
    return best_score;
}

SearchResult QuiescenceSearch::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    QParams p = QParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    auto t0 = std::chrono::high_resolution_clock::now();

    if(!state->legal_actions.size()) state->get_legal_actions();
    if(!state->legal_actions.empty()) result.best_move = state->legal_actions[0];

    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    const int ASPIRATION = 50; 

    std::vector<std::pair<int, Move>> root_scored;
    root_scored.reserve(state->legal_actions.size());
    for(auto &action : state->legal_actions){
        State* next = state->next_state(action);
        int quick = next->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        bool same = next->same_player_as_parent();
        int qscore = same ? quick : -quick;
        qscore += state->move_order_score(action, next);
        root_scored.emplace_back(qscore, action);
        delete next;
    }
    std::sort(root_scored.begin(), root_scored.end(), [](const auto &a, const auto &b){ return a.first > b.first; });

    int alpha = M_MAX;
    int beta = P_MAX;

    for(auto &ps : root_scored){
        Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int raw_score;
        int local_alpha = (best_score == M_MAX - 10) ? alpha : std::max(M_MAX, best_score - ASPIRATION);
        int local_beta  = (best_score == M_MAX - 10) ? beta  : std::min(P_MAX, best_score + ASPIRATION);

        if(same){
            raw_score = eval_ctx(next, depth - 1, history, 1, ctx, local_alpha, local_beta, p);
            if(raw_score <= local_alpha || raw_score >= local_beta){
                raw_score = eval_ctx(next, depth - 1, history, 1, ctx, alpha, beta, p);
            }
            int score = raw_score;
            if(score > best_score){
                best_score = score;
                result.best_move = action;
                if(p.report_partial && ctx.on_root_update){
                    ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
                }
            }
            if(best_score > alpha) alpha = best_score;
        } else {
            raw_score = eval_ctx(next, depth - 1, history, 1, ctx, -local_beta, -local_alpha, p);
            if(raw_score <= -local_beta || raw_score >= -local_alpha){
                raw_score = eval_ctx(next, depth - 1, history, 1, ctx, -beta, -alpha, p);
            }
            int score = -raw_score;
            if(score > best_score){
                best_score = score;
                result.best_move = action;
                if(p.report_partial && ctx.on_root_update){
                    ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
                }
            }
            if(best_score > alpha) alpha = best_score;
        }

        delete next;
        move_index++;
        if(ctx.stop) break;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.score = best_score;
    result.nodes = ctx.nodes;
    result.seldepth = ctx.seldepth;
    result.time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    return result;
}

ParamMap QuiescenceSearch::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"UseTT", "false"},
        {"UseQSearch", "true"}
    };
}

std::vector<ParamDef> QuiescenceSearch::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "false"},
        {"UseQSearch", ParamDef::CHECK, "true"}
    };
}