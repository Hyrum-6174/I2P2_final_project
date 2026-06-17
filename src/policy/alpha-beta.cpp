#include <algorithm>
#include <vector>
#include <utility>
#include <chrono>
#include "alpha-beta.hpp"
#include "state.hpp"

/*
 * Simple Alpha-Beta implementation with:
 * - move ordering via a quick static evaluation
 * - a lightweight killer heuristic (per-ply last cutoff move)
 * - basic aspiration window around the current best root score
 *
 * The search follows the project's `eval_ctx` contract: returned scores
 * are from the perspective of the `state` passed in (i.e. side-to-move).
 */

int AlphaBeta::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const ABParams& p
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

    history.push(state->hash());

    if(depth <= 0){
        int score = state->evaluate(p.use_kp_eval, p.use_eval_mobility, &history);
        history.pop(state->hash());
        return score;
    }

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
        // prefer killer move very highly
        if(killers[ply] == action){
            qscore += 100000; // ensure killer goes first
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
            // same side to move as parent: don't negate and keep window as-is
            int child_score = eval_ctx(next, depth - 1, history, ply + 1, ctx, alpha, beta, p);
            score = child_score;
        } else {
            // alternating player: use negamax window
            int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, p);
            score = -raw;
        }

        delete next;

        if(score > best_score) best_score = score;
        if(score > alpha) alpha = score;
        if(alpha >= beta){
            // record killer for this ply
            killers[ply] = action;
            break; // beta cutoff
        }
    }

    history.pop(state->hash());
    return best_score;
}

SearchResult AlphaBeta::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    ABParams p = ABParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    auto t0 = std::chrono::high_resolution_clock::now();

    if(!state->legal_actions.size()) state->get_legal_actions();
    if(!state->legal_actions.empty()) result.best_move = state->legal_actions[0];

    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();

    const int ASPIRATION = 50; // small window around current best

    // Order root moves by quick static eval (like in eval_ctx)
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
        // Try aspiration window around current best if we already have a candidate
        int local_alpha = (best_score == M_MAX - 10) ? alpha : std::max(M_MAX, best_score - ASPIRATION);
        int local_beta  = (best_score == M_MAX - 10) ? beta  : std::min(P_MAX, best_score + ASPIRATION);

        if(same){
            raw_score = eval_ctx(next, depth - 1, history, 1, ctx, local_alpha, local_beta, p);
            // If window failed (score outside), re-search with full window
            if(raw_score <= local_alpha || raw_score >= local_beta){
                raw_score = eval_ctx(next, depth - 1, history, 1, ctx, alpha, beta, p);
            }
            // raw_score is from parent's perspective already
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

ParamMap AlphaBeta::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"UseTT", "false"},
    };
}

std::vector<ParamDef> AlphaBeta::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "false"},
    };
}
