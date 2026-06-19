#include <algorithm>
#include <vector>
#include <utility>
#include <chrono>
#include "PVS.hpp"
#include "state.hpp"

int PVS::eval_ctx(
    State *state,
    int depth,
    GameHistory& history,
    int ply,
    SearchContext& ctx,
    int alpha,
    int beta,
    const PVSParams& p
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

    static thread_local std::vector<Move> killers(256);
    if((int)killers.size() <= ply) killers.resize(ply + 1);

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
    bool first_child = true;

    for(auto &ps : scored){
        const Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(first_child){
            if(same){
                score = eval_ctx(next, depth - 1, history, ply + 1, ctx, alpha, beta, p);
            } else {
                int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, p);
                score = -raw;
            }
            first_child = false;
        } else {
            if(same){
                int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, alpha, alpha + 1, p);
                score = raw;
                if(raw > alpha && raw < beta){
                    score = eval_ctx(next, depth - 1, history, ply + 1, ctx, alpha, beta, p);
                }
            } else {
                int raw = eval_ctx(next, depth - 1, history, ply + 1, ctx, -alpha - 1, -alpha, p);
                score = -raw;
                if(score > alpha && score < beta){
                    int raw2 = eval_ctx(next, depth - 1, history, ply + 1, ctx, -beta, -alpha, p);
                    score = -raw2;
                }
            }
        }

        delete next;

        if(score > best_score){
            best_score = score;
        }
        if(score > alpha) alpha = score;
        if(alpha >= beta){
            killers[ply] = action;
            break;
        }
    }

    history.pop(state->hash());
    return alpha;
}

SearchResult PVS::search(
    State *state,
    int depth,
    GameHistory& history,
    SearchContext& ctx
){
    ctx.reset();
    PVSParams p = PVSParams::from_map(ctx.params);
    SearchResult result;
    result.depth = depth;

    auto t0 = std::chrono::high_resolution_clock::now();

    if(!state->legal_actions.size()) state->get_legal_actions();
    if(!state->legal_actions.empty()) result.best_move = state->legal_actions[0];

    int best_score = M_MAX - 10;
    int move_index = 0;
    int total_moves = (int)state->legal_actions.size();
    int alpha = M_MAX;
    int beta = P_MAX;

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
    std::sort(root_scored.begin(), root_scored.end(), [](const auto &a, const auto &b){
        return a.first > b.first;
    });

    for(auto &ps : root_scored){
        Move action = ps.second;
        State* next = state->next_state(action);
        bool same = next->same_player_as_parent();

        int score;
        if(best_score == M_MAX - 10){
            if(same){
                score = eval_ctx(next, depth - 1, history, 1, ctx, alpha, beta, p);
            } else {
                int raw = eval_ctx(next, depth - 1, history, 1, ctx, -beta, -alpha, p);
                score = -raw;
            }
        } else {
            if(same){
                int raw = eval_ctx(next, depth - 1, history, 1, ctx, alpha, alpha + 1, p);
                score = raw;
                if(raw > alpha && raw < beta){
                    score = eval_ctx(next, depth - 1, history, 1, ctx, alpha, beta, p);
                }
            } else {
                int raw = eval_ctx(next, depth - 1, history, 1, ctx, -alpha - 1, -alpha, p);
                score = -raw;
                if(score > alpha && score < beta){
                    int raw2 = eval_ctx(next, depth - 1, history, 1, ctx, -beta, -alpha, p);
                    score = -raw2;
                }
            }
        }

        delete next;

        if(score > best_score){
            best_score = score;
            result.best_move = action;
            if(p.report_partial && ctx.on_root_update){
                ctx.on_root_update({result.best_move, best_score, depth, move_index + 1, total_moves});
            }
        }
        if(best_score > alpha) alpha = best_score;

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

ParamMap PVS::default_params(){
    return {
        {"UseKPEval", "true"},
        {"UseEvalMobility", "true"},
        {"ReportPartial", "true"},
        {"UseTT", "false"},
    };
}

std::vector<ParamDef> PVS::param_defs(){
    return {
        {"UseKPEval", ParamDef::CHECK, "true"},
        {"UseEvalMobility", ParamDef::CHECK, "true"},
        {"ReportPartial", ParamDef::CHECK, "true"},
        {"UseTT", ParamDef::CHECK, "false"},
    };
}
