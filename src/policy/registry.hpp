#pragma once
/*============================================================
 * Algorithm Registry
 *
 * Each algorithm defines:
 *   - search() function
 *   - default_params() returning ParamMap
 *   - param_defs() for UCI option advertisement
 *============================================================*/

#include <string>
#include <functional>
#include <vector>
#include "search_types.hpp"
#include "game_history.hpp"
#include "minimax.hpp"
#include "alpha-beta.hpp"
#include "random.hpp"
#include "PVS.hpp"
#include "Quiescence.hpp"
#include "mtdf_id.hpp"
#include "beth.hpp"

struct AlgoEntry {
    std::string name;
    ParamMap default_params;
    std::vector<ParamDef> param_defs;
    std::function<SearchResult(State*, int, GameHistory&, SearchContext&)> search;
};

inline const std::vector<AlgoEntry>& get_algo_table(){
    static const std::vector<AlgoEntry> table = {
        {
            "minimax",
            MiniMax::default_params(),
            MiniMax::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return MiniMax::search(s, d, h, c);
            }
        },
        {
            "alpha-beta",
            AlphaBeta::default_params(),
            AlphaBeta::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return AlphaBeta::search(s, d, h, c);
            }
        },
        {
            "pvs",
            PVS::default_params(),
            PVS::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return PVS::search(s, d, h, c);
            }
        },
        {
            "quiescence",
            QuiescenceSearch::default_params(),
            QuiescenceSearch::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return QuiescenceSearch::search(s, d, h, c);
            }
        },
        {
            "mtdf-id",
            MTDF_ID::default_params(),
            MTDF_ID::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return MTDF_ID::search(s, d, h, c);
            }
        },
        {
            "beth",
            BethSearch::default_params(),
            BethSearch::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return BethSearch::search(s, d, h, c);
            }
        },
        {
            "random",
            Random::default_params(),
            Random::param_defs(),
            [](State* s, int d, GameHistory& h, SearchContext& c){
                return Random::search(s, d, h, c);
            }
        },
    };
    return table;
}

inline const AlgoEntry* find_algo(const std::string& name){
    for(auto& entry : get_algo_table()){
        if(entry.name == name){
            return &entry;
        }
    }
    return nullptr;
}

inline std::string default_algo_name(){ return "minimax"; }
