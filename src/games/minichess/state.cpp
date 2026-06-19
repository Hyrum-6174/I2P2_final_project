#include <iostream>
#include <sstream>
#include <cstdint>
#include <cstdlib>

#include "./state.hpp"
#include "config.hpp"
#include "../../policy/game_history.hpp"


/*============================================================
 * KP (King-Piece) Evaluation tables
 *
 * Always compiled. Toggled at runtime via use_kp_eval param.
 *============================================================*/

// KP material (10x scale for fine positional granularity)
static const int kp_material[7] = {0, 20, 60, 70, 80, 200, 1000};

// Material-only (simple scale)
static const int simple_material[7] = {0, 2, 6, 7, 8, 20, 100};

// Piece-Square Tables (white perspective, mirror for black)
static const int pst[6][BOARD_H][BOARD_W] = {
    // Pawn
    {{ 0,  0,  0,  0,  0}, {15, 15, 15, 15, 15}, { 4,  6, 10,  6,  4},
     { 2,  4,  6,  4,  2}, { 0,  2,  2,  2,  0}, { 0,  0,  0,  0,  0}},
    // Rook
    {{ 2,  2,  2,  2,  2}, { 4,  4,  4,  4,  4}, { 0,  0,  2,  0,  0},
     { 0,  0,  2,  0,  0}, { 0,  0,  2,  0,  0}, { 0,  0,  0,  0,  0}},
    // Knight
    {{-4, -2,  0, -2, -4}, {-2,  2,  4,  2, -2}, { 0,  4,  6,  4,  0},
     { 0,  4,  6,  4,  0}, {-2,  2,  4,  2, -2}, {-4, -2,  0, -2, -4}},
    // Bishop
    {{-2,  0,  0,  0, -2}, { 0,  3,  4,  3,  0}, { 0,  4,  4,  4,  0},
     { 0,  4,  4,  4,  0}, { 0,  3,  4,  3,  0}, {-2,  0,  0,  0, -2}},
    // Queen
    {{-2,  0,  2,  0, -2}, { 0,  2,  4,  2,  0}, { 0,  4,  6,  4,  0},
     { 0,  4,  6,  4,  0}, { 0,  2,  4,  2,  0}, {-2,  0,  2,  0, -2}},
    // King
    {{-8, -8, -8, -8, -8}, {-4, -4, -4, -4, -4}, {-4, -4, -4, -4, -4},
     {-4, -4, -4, -4, -4}, { 4,  4,  0,  4,  4}, { 6,  6,  2,  6,  6}},
};

// King tropism weights
static const int tropism_w[7] = {0, 0, 3, 3, 2, 5, 0};

static int king_tropism(
    int piece_type,
    int pr, int pc,
    int ekr, int ekc
){
    int dist = std::max(std::abs(pr - ekr), std::abs(pc - ekc));
    if(dist <= 2){
        return tropism_w[piece_type] * (3 - dist);
    }
    return 0;
}

static bool king_tropism_applicable(
    const State* state,
    int piece_type,
    int pr, int pc,
    int player
){
    if(piece_type <= 0 || piece_type > 6) return false;

    int enemy = 1 - player;
    int attackers = state->count_attackers_on_square(pr, pc, enemy);
    if(attackers == 0){
        return true;
    }

    int defenders = state->count_defenders_on_square(pr, pc, player);
    if(defenders >= attackers){
        return true;
    }


    return false;
}


/*============================================================
 * Fork Detection Helpers
 *
 * Helper functions to detect forks (pieces attacking 2+ enemy pieces)
 * and assess their defensibility.
 *============================================================*/

/* Count how many opponent pieces are attacked by a piece at (from_r, from_c).
 * Returns count of enemy pieces that would be capturable by this piece. */
int State::count_attacked_pieces(int attacker_r, int attacker_c, int player) const {
    auto self_board = this->board.board[player];
    auto oppn_board = this->board.board[1 - player];
    int piece = self_board[attacker_r][attacker_c];
    int count = 0;

    if(piece <= 0 || piece > 6) return 0;

    // Pawn
    if(piece == 1){
        if(player == 0){
            // White pawn attacks diagonally upward (row-1)
            if(attacker_r > 0){
                if(attacker_c > 0 && oppn_board[attacker_r - 1][attacker_c - 1] > 0) count++;
                if(attacker_c < BOARD_W - 1 && oppn_board[attacker_r - 1][attacker_c + 1] > 0) count++;
            }
        } else {
            // Black pawn attacks diagonally downward (row+1)
            if(attacker_r < BOARD_H - 1){
                if(attacker_c > 0 && oppn_board[attacker_r + 1][attacker_c - 1] > 0) count++;
                if(attacker_c < BOARD_W - 1 && oppn_board[attacker_r + 1][attacker_c + 1] > 0) count++;
            }
        }
    }
    // Knight
    else if(piece == 3){
        int kr[] = {-1, -1, 1, 1, 2, 2, -2, -2};
        int kc[] = {2, -2, 2, -2, 1, -1, 1, -1};
        for(int i = 0; i < 8; i++){
            int nr = attacker_r + kr[i];
            int nc = attacker_c + kc[i];
            if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                if(oppn_board[nr][nc] > 0) count++;
            }
        }
    }
    // King
    else if(piece == 6){
        for(int dr = -1; dr <= 1; dr++){
            for(int dc = -1; dc <= 1; dc++){
                if(dr == 0 && dc == 0) continue;
                int nr = attacker_r + dr;
                int nc = attacker_c + dc;
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    if(oppn_board[nr][nc] > 0) count++;
                }
            }
        }
    }
    // Rook, Bishop, Queen (sliding pieces)
    else if(piece == 2 || piece == 4 || piece == 5){
        // Rook: 4 directions (0-3), Bishop: 4 diagonals (4-7), Queen: all 8
        int d_start = (piece == 4) ? 4 : 0;
        int d_end = (piece == 2) ? 4 : 8;
        
        for(int d = d_start; d < d_end; d++){
            int dr[] = {0, 0, 1, -1, 1, 1, -1, -1};
            int dc[] = {1, -1, 0, 0, 1, -1, 1, -1};
            
            int nr = attacker_r + dr[d];
            int nc = attacker_c + dc[d];
            
            while(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                if(self_board[nr][nc] > 0) break; // own piece blocks
                if(oppn_board[nr][nc] > 0){
                    count++;
                    break; // can only attack one piece per direction
                }
                nr += dr[d];
                nc += dc[d];
            }
        }
    }

    return count;
}

/* Check if a piece at (defend_r, defend_c) is defended by any friendly piece. */
bool State::is_defended(int defend_r, int defend_c, int player) const {
    auto self_board = this->board.board[player];
    
    // Check all friendly pieces to see if any can attack this square
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = self_board[r][c];
            if(piece <= 0 || piece > 6 || (r == defend_r && c == defend_c)) continue;
            
            // Check if this piece can defend (attack) the target square
            if(piece == 1){ // Pawn
                if(player == 0){
                    if(r - 1 == defend_r && (c - 1 == defend_c || c + 1 == defend_c)) return true;
                } else {
                    if(r + 1 == defend_r && (c - 1 == defend_c || c + 1 == defend_c)) return true;
                }
            }
            else if(piece == 3){ // Knight
                int dr = std::abs(r - defend_r);
                int dc = std::abs(c - defend_c);
                if((dr == 2 && dc == 1) || (dr == 1 && dc == 2)) return true;
            }
            else if(piece == 6){ // King
                if(std::abs(r - defend_r) <= 1 && std::abs(c - defend_c) <= 1) return true;
            }
            else if(piece == 2 || piece == 4 || piece == 5){ // Rook, Bishop, Queen
                int d_start = (piece == 4) ? 4 : 0;
                int d_end = (piece == 2) ? 4 : 8;
                
                for(int d = d_start; d < d_end; d++){
                    int dr[] = {0, 0, 1, -1, 1, 1, -1, -1};
                    int dc[] = {1, -1, 0, 0, 1, -1, 1, -1};
                    
                    int nr = r + dr[d];
                    int nc = c + dc[d];
                    
                    while(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                        if(nr == defend_r && nc == defend_c) return true;
                        if(self_board[nr][nc] > 0) break; // own piece blocks
                        nr += dr[d];
                        nc += dc[d];
                    }
                }
            }
        }
    }
    return false;
}

int State::count_attackers_on_square(int target_r, int target_c, int attacker_player) const {
    int count = 0;
    auto attacker_board = this->board.board[attacker_player];
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = attacker_board[r][c];
            if(piece > 0 && piece <= 6){
                if(attacks_square(r, c, attacker_player, target_r, target_c)){
                    count++;
                }
            }
        }
    }
    return count;
}

int State::count_defenders_on_square(int target_r, int target_c, int defender_player) const {
    return count_attackers_on_square(target_r, target_c, defender_player);
}

int State::space_score(int player) const {
    State temp(this->board, player);
    temp.get_legal_actions();
    return temp.legal_actions.size();
}

int State::pawn_structure_score(int pr, int pc, int player) const {
    if(this->board.board[player][pr][pc] != 1) return 0;

    int score = 0;
    int back_r = (player == 0) ? pr + 1 : pr - 1;
    if(back_r >= 0 && back_r < BOARD_H){
        if(pc > 0 && this->board.board[player][back_r][pc - 1] == 1){
            score += 4;
        }
        if(pc < BOARD_W - 1 && this->board.board[player][back_r][pc + 1] == 1){
            score += 4;
        }
    }

    int forward_r = (player == 0) ? pr - 1 : pr + 1;
    if(forward_r >= 0 && forward_r < BOARD_H){
        for(int dc = -1; dc <= 1; dc += 2){
            int nc = pc + dc;
            if(nc < 0 || nc >= BOARD_W) continue;
            if(this->board.board[1 - player][forward_r][nc] > 0){
                score += 3;
            } else {
                score += 1;
            }
        }
    }

    return score;
}

int State::king_threat_score(int king_r, int king_c, int owner, int attacker) const {
    if(king_r < 0 || king_c < 0) return 0;
    int score = 0;
    int king_attackers = count_attackers_on_square(king_r, king_c, attacker);
    if(king_attackers > 0){
        score += 30 + king_attackers * 10;
    }

    for(int dr = -2; dr <= 2; dr++){
        for(int dc = -2; dc <= 2; dc++){
            if(dr == 0 && dc == 0) continue;
            int r = king_r + dr;
            int c = king_c + dc;
            if(r < 0 || r >= BOARD_H || c < 0 || c >= BOARD_W) continue;
            int piece = this->board.board[owner][r][c];
            if(piece <= 0 || piece == 6) continue;
            int attackers = count_attackers_on_square(r, c, attacker);
            if(attackers == 0) continue;
            if(!is_defended(r, c, owner)){
                score += kp_material[piece] + 8;
            } else {
                score += 4;
            }
        }
    }
    return score;
}

bool State::attacks_square(int r, int c, int player, int target_r, int target_c) const {
    if(r == target_r && c == target_c) return false;
    auto self_board = this->board.board[player];
    auto opp_board = this->board.board[1 - player];
    int piece = self_board[r][c];
    if(piece <= 0 || piece > 6) return false;

    int dr = target_r - r;
    int dc = target_c - c;
    int adr = std::abs(dr);
    int adc = std::abs(dc);

    if(piece == 1){
        if(player == 0){
            return dr == -1 && adc == 1;
        }
        return dr == 1 && adc == 1;
    }
    if(piece == 3){
        return (adr == 2 && adc == 1) || (adr == 1 && adc == 2);
    }
    if(piece == 6){
        return std::max(adr, adc) == 1;
    }
    int drs[8] = {0, 0, 1, -1, 1, 1, -1, -1};
    int dcs[8] = {1, -1, 0, 0, 1, -1, 1, -1};
    int start = (piece == 4) ? 4 : 0;
    int end = (piece == 2) ? 4 : 8;
    for(int d = start; d < end; d++){
        int step_r = drs[d];
        int step_c = dcs[d];
        if(step_r == 0 && step_c == 0) continue;
        int nr = r + step_r;
        int nc = c + step_c;
        while(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
            if(nr == target_r && nc == target_c) return true;
            if(self_board[nr][nc] > 0 || opp_board[nr][nc] > 0) break;
            nr += step_r;
            nc += step_c;
        }
    }
    return false;
}

int State::move_order_score(const Move& action, const State* next) const {
    int score = 0;
    int from_r = action.first.first;
    int from_c = action.first.second;
    int enemy = 1 - this->player;
    bool from_attacked = this->count_attackers_on_square(from_r, from_c, enemy) > 0;
    if(!from_attacked){
        return 0;
    }

    int dest_r = action.second.first;
    int dest_c = action.second.second;
    int captured = this->board.board[enemy][dest_r][dest_c];
    if(captured > 0){
        score += 8;
    }

    bool discovered = false;
    for(int r = 0; r < BOARD_H && !discovered; r++){
        for(int c = 0; c < BOARD_W && !discovered; c++){
            if(r == from_r && c == from_c) continue;
            int piece = this->board.board[this->player][r][c];
            if(piece <= 0) continue;
            for(int er = 0; er < BOARD_H && !discovered; er++){
                for(int ec = 0; ec < BOARD_W; ec++){
                    int target_piece = next->board.board[enemy][er][ec];
                    if(target_piece <= 0) continue;
                    if(next->attacks_square(r, c, this->player, er, ec) &&
                       !this->attacks_square(r, c, this->player, er, ec)){
                        discovered = true;
                        break;
                    }
                }
            }
        }
    }
    if(discovered){
        score += 4;
    }

    // If the move gives check in the next position, prefer it slightly.
    int opp = next->player;
    int king_r = -1, king_c = -1;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            if(next->board.board[opp][r][c] == 6){
                king_r = r;
                king_c = c;
                break;
            }
        }
        if(king_r != -1) break;
    }
    if(king_r != -1 && next->count_attackers_on_square(king_r, king_c, 1 - opp) > 0){
        score += 5;
    }

    // Prefer escaping to a less attacked square if no capture/check/discovery is available.
    if(score == 0){
        int future_attackers = next->count_attackers_on_square(dest_r, dest_c, enemy);
        if(future_attackers == 0){
            score += 2;
        }
    }

    return score;
}

bool State::is_pinned_piece(int piece_r, int piece_c, int target_player) const {
    int piece = this->board.board[target_player][piece_r][piece_c];
    if(piece <= 0 || piece == 6) return false;
    int king_r = -1, king_c = -1;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            if(this->board.board[target_player][r][c] == 6){
                king_r = r;
                king_c = c;
                break;
            }
        }
        if(king_r != -1) break;
    }
    if(king_r == -1) return false;

    int dr = piece_r - king_r;
    int dc = piece_c - king_c;
    if(dr == 0 && dc == 0) return false;
    int step_r = (dr > 0) ? 1 : ((dr < 0) ? -1 : 0);
    int step_c = (dc > 0) ? 1 : ((dc < 0) ? -1 : 0);
    if(step_r != 0 && step_c != 0 && std::abs(dr) != std::abs(dc)) return false;
    if(step_r == 0 && step_c == 0) return false;

    int nr = king_r + step_r;
    int nc = king_c + step_c;
    bool seen_target = false;
    while(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
        int occupant = this->board.board[target_player][nr][nc] ? this->board.board[target_player][nr][nc] : this->board.board[1 - target_player][nr][nc];
        if(nr == piece_r && nc == piece_c){
            if(occupant != piece) return false;
            seen_target = true;
            nr += step_r;
            nc += step_c;
            continue;
        }
        if(occupant > 0){
            if(!seen_target) return false;
            int attacker = this->board.board[1 - target_player][nr][nc];
            if(attacker == 2 && step_r * step_c == 0) return true;
            if(attacker == 4 && step_r != 0 && step_c != 0) return true;
            if(attacker == 5) return true;
            return false;
        }
        nr += step_r;
        nc += step_c;
    }
    return false;
}


/*============================================================
 * evaluate() — runtime-selectable eval strategy
 *============================================================*/

int State::evaluate(
    bool use_kp_eval,
    bool use_mobility,
    const GameHistory* history
){
    (void)history; // just to suppress warning

    // [ Hackathon TODO 1-1 ]
    // if in win state, return max score(you can check base_state.hpp for max score)
    if(this->game_state == WIN){
        return P_MAX;
    }


    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];
    int self_score = 0, oppn_score = 0;
    int self_material = 0, oppn_material = 0;
    int self_pawn_structure = 0, oppn_pawn_structure = 0;
    int self_king_threat = 0, oppn_king_threat = 0;

    if(use_kp_eval){
        /* === KP eval: material + PST + tropism === */

        int self_kr = -1, self_kc = -1;
        int oppn_kr = -1, oppn_kc = -1;
        // [ Hackathon TODO 1-3 ]
        // get the position for player's king and opponent's king

        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                if(self_board[r][c] == 6) { self_kr = r; self_kc = c; }
                if(oppn_board[r][c] == 6) { oppn_kr = r; oppn_kc = c; }
            }
        }

        self_king_threat = king_threat_score(self_kr, self_kc, this->player, 1 - this->player);
        oppn_king_threat = king_threat_score(oppn_kr, oppn_kc, 1 - this->player, this->player);

        // [ Hackathon TODO 1-4 ]
        // sum player/opponent pieces' value and add to score
        // if enemy king is still on the board, you should also call king_tropism for your pieces and add the value to score
        // king_tropism is already given above

        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int self_piece = self_board[r][c];
                if(self_piece > 0 && self_piece <= 6){
                    // White orientation index mapping for PST (mirror for black player index 1)
                    int p_row = (this->player == 0) ? r : (BOARD_H - 1 - r);
                    self_score += kp_material[self_piece];
                    self_score += pst[self_piece - 1][p_row][c];
                    self_material += kp_material[self_piece];
                    if(self_piece == 1){
                        self_pawn_structure += this->pawn_structure_score(r, c, this->player);
                    }
                    
                    if(oppn_kr != -1 && king_tropism_applicable(this, self_piece, r, c, this->player)){
                        self_score += king_tropism(self_piece, r, c, oppn_kr, oppn_kc);
                    }
                }

                int oppn_piece = oppn_board[r][c];
                if(oppn_piece > 0 && oppn_piece <= 6){
                    int o_row = (this->player == 1) ? r : (BOARD_H - 1 - r);
                    oppn_score += kp_material[oppn_piece];
                    oppn_score += pst[oppn_piece - 1][o_row][c];
                    oppn_material += kp_material[oppn_piece];
                    if(oppn_piece == 1){
                        oppn_pawn_structure += this->pawn_structure_score(r, c, 1 - this->player);
                    }
                    
                    if(self_kr != -1 && king_tropism_applicable(this, oppn_piece, r, c, 1 - this->player)){
                        oppn_score += king_tropism(oppn_piece, r, c, self_kr, self_kc);
                    }
                }
            }
        }


    }else{
        /* === Simple material-only eval === */

        // [ Hackathon TODO 1-2 ]
        // Simply add each piece's value to score
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int self_piece = self_board[r][c];
                if(self_piece > 0 && self_piece <= 6){
                    self_score += simple_material[self_piece];
                }
                int oppn_piece = oppn_board[r][c];
                if(oppn_piece > 0 && oppn_piece <= 6){
                    oppn_score += simple_material[oppn_piece];
                }
            }
        }

    }

    int bonus = 0;

    if(use_kp_eval && self_material == oppn_material){
        int self_space = this->space_score(this->player);
        int oppn_space = this->space_score(1 - this->player);
        bonus += self_space - oppn_space;
    }

    bonus += self_pawn_structure - oppn_pawn_structure;
    bonus += oppn_king_threat - self_king_threat;

    /* === Mobility bonus === */
    if(use_mobility){
        // [ Hackathon TODO 1-5 ]
        // you can calculate mobility by legal actions size
        // bonus += 2 * (self_mobility - oppn_mobility);
        int current_player = this->player;
        auto current_actions = this->legal_actions;
        auto current_state = this->game_state;

        if(current_actions.empty() && current_state == UNKNOWN) {
            const_cast<State*>(this)->get_legal_actions();
        }
        int self_mobility = this->legal_actions.size();

        // Evaluate opponent's legal moves from their position perspective
        State opp_state(this->board, 1 - current_player);
        opp_state.get_legal_actions();
        int oppn_mobility = opp_state.legal_actions.size();

        // Restore our action cache mutations if necessary
        const_cast<State*>(this)->legal_actions = current_actions;
        const_cast<State*>(this)->game_state = current_state;

        bonus += 2 * (self_mobility - oppn_mobility);
    }

    /* === Fork Detection and Evaluation === */
    // Evaluate our forking opportunities
    int self_fork_bonus = 0;
    int oppn_fork_penalty = 0;
    int hanging_piece_penalty = 0;
    
    // Check our pieces for forks
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = self_board[r][c];
            if(piece > 0 && piece <= 5){ // exclude king
                int attackers = const_cast<State*>(this)->count_attackers_on_square(r, c, 1 - this->player);
                int defenders = const_cast<State*>(this)->count_defenders_on_square(r, c, this->player);
                if (attackers > defenders) {
                    // Heavily penalize leaving undefended or overwhelmed pieces on the board
                    hanging_piece_penalty += (kp_material[piece] / 2);
                }
                
                int attacked = const_cast<State*>(this)->count_attacked_pieces(r, c, this->player);
                
                // If this piece forks (attacks 2+ pieces), reward it
                if(attacked >= 2){
                    // Base fork bonus (higher for more pieces attacked)
                    int fork_value = 15 + (attacked - 2) * 5;
                    
                    // If the forking piece is defended, increase the bonus
                    if(const_cast<State*>(this)->is_defended(r, c, this->player)){
                        fork_value += 10;
                        self_fork_bonus += fork_value;
                    }
                }
            }
        }
    }
    
    // Check opponent's pieces for fork vulnerability (penalize if we're vulnerable)
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = oppn_board[r][c];
            if(piece > 0 && piece <= 5){ // exclude king
                int attackers = const_cast<State*>(this)->count_attackers_on_square(r, c, this->player);
                int defenders = const_cast<State*>(this)->count_defenders_on_square(r, c, 1 - this->player);
                if (attackers > defenders) {
                    // Reward us if the opponent leaves a piece hanging
                    hanging_piece_penalty -= (kp_material[piece] / 2);
                }
                
                int attacked = const_cast<State*>(this)->count_attacked_pieces(r, c, 1 - this->player);
                
                // If opponent can fork our pieces, penalize us
                if(attacked >= 2){
                    int fork_vulnerability = 15 + (attacked - 2) * 5;
                    
                    // If opponent's forking piece is defended, penalize more
                    if(const_cast<State*>(this)->is_defended(r, c, 1 - this->player)){
                        fork_vulnerability += 10;
                        oppn_fork_penalty += fork_vulnerability;
                    }
                }
            }
        }
    }

    // Check for pinned enemy pieces and pressure value
    int pin_pressure_bonus = 0;
    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int piece = oppn_board[r][c];
            if(piece > 0 && piece != 6 && const_cast<State*>(this)->is_pinned_piece(r, c, 1 - this->player)){
                int attackers = const_cast<State*>(this)->count_attackers_on_square(r, c, this->player);
                int defenders = const_cast<State*>(this)->count_defenders_on_square(r, c, 1 - this->player);
                int pressure = 5;

                if(attackers > defenders + 1){
                    pressure += 15 + (attackers - defenders - 1) * 5;
                } else if(attackers > defenders){
                    pressure += 10;
                } else if(attackers == defenders){
                    pressure += 5;
                }

                if(!const_cast<State*>(this)->is_defended(r, c, 1 - this->player)){
                    pressure += 5;
                }

                // If our attacking force is significantly stronger than the defenders,
                // reward this pin as a likely safe target rather than poison.
                if(attackers >= defenders + 2){
                    pressure += 10;
                }

                pin_pressure_bonus += pressure;
            }
        }
    }

    bonus += self_fork_bonus - oppn_fork_penalty + pin_pressure_bonus;

    return self_score - oppn_score + bonus;
}



/*============================================================
 * Zobrist hash for transposition table
 *============================================================*/
static uint64_t zobrist_piece[2][7][BOARD_H][BOARD_W];
static uint64_t zobrist_side;
static bool zobrist_ready = false;

static void init_zobrist(){
    uint64_t s = 0x7A35C9D1E4F02B68ULL;
    auto rand64 = [&s]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for(int p = 0; p < 2; p++){
        for(int t = 0; t < 7; t++){
            for(int r = 0; r < BOARD_H; r++){
                for(int c = 0; c < BOARD_W; c++){
                    zobrist_piece[p][t][r][c] = rand64();
                }
            }
        }
    }
    zobrist_side = rand64();
    zobrist_ready = true;
}

uint64_t State::compute_hash_full() const{
    if(!zobrist_ready){
        init_zobrist();
    }
    uint64_t h = 0;
    for(int p = 0; p < 2; p++){
        for(int r = 0; r < BOARD_H; r++){
            for(int c = 0; c < BOARD_W; c++){
                int piece = this->board.board[p][r][c];
                if(piece){
                    h ^= zobrist_piece[p][piece][r][c];
                }
            }
        }
    }
    if(this->player){
        h ^= zobrist_side;
    }
    return h;
}


/**
 * @brief return next state after the move
 *
 * @param move
 * @return State*
 */
State* State::next_state(const Move& move){
    if(!zobrist_ready){ init_zobrist(); }

    Board next = this->board;
    Point from = move.first, to = move.second;
    int p = this->player;
    int opp = 1 - p;

    int8_t orig_piece = next.board[p][from.first][from.second];
    int8_t moved = orig_piece;
    //promotion for pawn
    if(moved == 1 && (to.first==BOARD_H-1 || to.first==0)){
        moved = 5;
    }

    /* Incremental hash update */
    uint64_t h = this->hash();
    h ^= zobrist_side;  /* toggle side to move */

    /* XOR out piece from source */
    h ^= zobrist_piece[p][orig_piece][from.first][from.second];

    /* XOR out captured piece at destination */
    int8_t captured = next.board[opp][to.first][to.second];
    if(captured){
        h ^= zobrist_piece[opp][captured][to.first][to.second];
        next.board[opp][to.first][to.second] = 0;
    }

    /* XOR in piece at destination */
    h ^= zobrist_piece[p][moved][to.first][to.second];

    next.board[p][from.first][from.second] = 0;
    next.board[p][to.first][to.second] = moved;

    State* ns = new State(next, opp);
    ns->zobrist_hash = h;
    ns->zobrist_valid = true;
    return ns;
}


static const int move_table_rook_bishop[8][7][2] = {
  {{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7}},
  {{0, -1}, {0, -2}, {0, -3}, {0, -4}, {0, -5}, {0, -6}, {0, -7}},
  {{1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}, {6, 0}, {7, 0}},
  {{-1, 0}, {-2, 0}, {-3, 0}, {-4, 0}, {-5, 0}, {-6, 0}, {-7, 0}},
  {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}},
  {{1, -1}, {2, -2}, {3, -3}, {4, -4}, {5, -5}, {6, -6}, {7, -7}},
  {{-1, 1}, {-2, 2}, {-3, 3}, {-4, 4}, {-5, 5}, {-6, 6}, {-7, 7}},
  {{-1, -1}, {-2, -2}, {-3, -3}, {-4, -4}, {-5, -5}, {-6, -6}, {-7, -7}},
};

// [ Hackathon TODO 2-1 ]
// fill the knight move table
static const int move_table_knight[8][2] = {
  {-1, 2}, {1, 2}, {2, 1}, {2, -1},
  {-1, -2}, {1, -2}, {-2, 1}, {-2, -1}
};
static const int move_table_king[8][2] = {
  {1, 0}, {0, 1}, {-1, 0}, {0, -1}, 
  {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
};


/*============================================================
 * Naive move generation (array-based, branch-heavy)
 *============================================================*/
void State::get_legal_actions_naive(){
    this->game_state = NONE;
    std::vector<Move> all_actions;
    all_actions.reserve(64);
    auto self_board = this->board.board[this->player];
    auto oppn_board = this->board.board[1 - this->player];

    int now_piece, oppn_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece=self_board[i][j])){
                switch(now_piece){
                    case 1: //pawn
                        if(this->player && i<BOARD_H-1){
                            //black
                            if(!oppn_board[i+1][j] && !self_board[i+1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i+1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i+1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i+1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }else if(!this->player && i>0){
                            //white
                            if(!oppn_board[i-1][j] && !self_board[i-1][j]){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j)));
                            }
                            if(j<BOARD_W-1 && (oppn_piece=oppn_board[i-1][j+1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j+1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                            if(j>0 && (oppn_piece=oppn_board[i-1][j-1])>0){
                                all_actions.push_back(Move(Point(i, j), Point(i-1, j-1)));
                                if(oppn_piece==6){
                                    this->game_state = WIN;
                                    this->legal_actions = all_actions;
                                    return;
                                }
                            }
                        }
                        break;

                    case 2: //rook
                    case 4: //bishop
                    case 5: //queen
                        int st, end;
                        switch(now_piece){
                            case 2: st=0; end=4; break; //rook
                            case 4: st=4; end=8; break; //bishop
                            case 5: st=0; end=8; break; //queen
                            default: st=0; end=-1;
                        }
                        for(int part=st; part<end; part+=1){
                            auto move_list = move_table_rook_bishop[part];
                            for(int k=0; k<std::max(BOARD_H, BOARD_W); k+=1){
                                int p[2] = {move_list[k][0] + i, move_list[k][1] + j};

                                if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                    break;
                                }
                                now_piece = self_board[p[0]][p[1]];
                                if(now_piece){
                                    break;
                                }

                                all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                                oppn_piece = oppn_board[p[0]][p[1]];
                                if(oppn_piece){
                                    if(oppn_piece==6){
                                        this->game_state = WIN;
                                        this->legal_actions = all_actions;
                                        return;
                                    }else{
                                        break;
                                    }
                                };
                            }
                        }
                        break;

                    case 3: //knight
                        // [ Hackathon TODO 2-2 ]
                        // complete knight's movement, you can refer to other pieces' movement
                        for(auto move : move_table_knight){
                            int p[2] = {move[0] + i, move[1] + j};

                            // 1. Out of bounds check
                            if(p[0] >= BOARD_H || p[0] < 0 || p[1] >= BOARD_W || p[1] < 0){
                                continue;
                            }

                            // 2. Friendly fire check (cannot move to a square occupied by own piece)
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            // 3. Move is valid, add it to the list
                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            // 4. Winning condition check (capturing the opponent's king)
                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece == 6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;

                    case 6: //king
                        for(auto move: move_table_king){
                            int p[2] = {move[0] + i, move[1] + j};

                            if(p[0]>=BOARD_H || p[0]<0 || p[1]>=BOARD_W || p[1]<0){
                                continue;
                            }
                            now_piece = self_board[p[0]][p[1]];
                            if(now_piece){
                                continue;
                            }

                            all_actions.push_back(Move(Point(i, j), Point(p[0], p[1])));

                            oppn_piece = oppn_board[p[0]][p[1]];
                            if(oppn_piece==6){
                                this->game_state = WIN;
                                this->legal_actions = all_actions;
                                return;
                            }
                        }
                        break;
                }
            }
        }
    }
    this->legal_actions = all_actions;
}


/*============================================================
 * Bitboard move generation
 *
 * 6x5 = 30 squares fit in a uint32_t.
 * Square (r,c) -> bit index r*5+c.
 * Precomputed attack masks for leapers (knight, king, pawn).
 * Bit-scan loop (__builtin_ctz) replaces nested array iteration.
 *============================================================*/
#define BB_SQ(r, c)  ((r) * BOARD_W + (c))
#define BB_ROW(sq)   ((sq) / BOARD_W)
#define BB_COL(sq)   ((sq) % BOARD_W)

// Precomputed attack tables (initialized once)
static uint32_t bb_knight[30];       // knight attack mask per square
static uint32_t bb_king[30];         // king attack mask per square
static uint32_t bb_pawn_push[2][30]; // pawn push target per player/square
static uint32_t bb_pawn_cap[2][30];  // pawn capture targets per player/square
static bool bb_ready = false;

// Sliding piece direction vectors (0-3: rook, 4-7: bishop, 0-7: queen)
static const int bb_dr[8] = {0, 0, 1, -1, 1, 1, -1, -1};
static const int bb_dc[8] = {1, -1, 0, 0, 1, -1, 1, -1};

static void bb_init(){
    static const int kn_dr[8] = {1, 1, -1, -1, 2, 2, -2, -2};
    static const int kn_dc[8] = {2, -2, 2, -2, 1, -1, 1, -1};
    static const int ki_dr[8] = {1, 0, -1, 0, 1, 1, -1, -1};
    static const int ki_dc[8] = {0, 1, 0, -1, 1, -1, 1, -1};

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);

            // Knight
            bb_knight[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + kn_dr[d], nc = c + kn_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_knight[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // King
            bb_king[sq] = 0;
            for(int d = 0; d < 8; d++){
                int nr = r + ki_dr[d], nc = c + ki_dc[d];
                if(nr >= 0 && nr < BOARD_H && nc >= 0 && nc < BOARD_W){
                    bb_king[sq] |= 1u << BB_SQ(nr, nc);
                }
            }

            // Pawn (player 0 = white, advances up = row-1)
            bb_pawn_push[0][sq] = 0;
            bb_pawn_cap[0][sq] = 0;
            if(r > 0){
                bb_pawn_push[0][sq] = 1u << BB_SQ(r-1, c);
                if(c > 0){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[0][sq] |= 1u << BB_SQ(r-1, c+1);
                }
            }

            // Pawn (player 1 = black, advances down = row+1)
            bb_pawn_push[1][sq] = 0;
            bb_pawn_cap[1][sq] = 0;
            if(r < BOARD_H-1){
                bb_pawn_push[1][sq] = 1u << BB_SQ(r+1, c);
                if(c > 0){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c-1);
                }
                if(c < BOARD_W-1){
                    bb_pawn_cap[1][sq] |= 1u << BB_SQ(r+1, c+1);
                }
            }
        }
    }
    bb_ready = true;
}

void State::get_legal_actions_bitboard(){
    if(!bb_ready){
        bb_init();
    }

    this->game_state = NONE;
    this->legal_actions.clear();
    this->legal_actions.reserve(64);

    int self = this->player;
    int oppn = 1 - self;

    // Build occupancy bitmasks and piece-type lookup
    uint32_t self_occ = 0, oppn_occ = 0;
    int self_pt[30] = {};  // piece type at each square (self)
    int oppn_pt[30] = {};  // piece type at each square (opponent)

    for(int r = 0; r < BOARD_H; r++){
        for(int c = 0; c < BOARD_W; c++){
            int sq = BB_SQ(r, c);
            if(this->board.board[self][r][c]){
                self_occ |= 1u << sq;
                self_pt[sq] = this->board.board[self][r][c];
            }
            if(this->board.board[oppn][r][c]){
                oppn_occ |= 1u << sq;
                oppn_pt[sq] = this->board.board[oppn][r][c];
            }
        }
    }

    uint32_t all_occ = self_occ | oppn_occ;

    // Iterate own pieces via bit scan
    uint32_t pieces = self_occ;
    while(pieces){
        int sq = __builtin_ctz(pieces);
        pieces &= pieces - 1;
        int r = BB_ROW(sq), c = BB_COL(sq);
        int piece = self_pt[sq];
        uint32_t targets = 0;

        switch(piece){
            case 1: { // Pawn
                uint32_t push = bb_pawn_push[self][sq] & ~all_occ;
                uint32_t cap = bb_pawn_cap[self][sq] & oppn_occ;
                // Check for king capture in captures
                uint32_t cap_scan = cap;
                while(cap_scan){
                    int to = __builtin_ctz(cap_scan);
                    cap_scan &= cap_scan - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                targets = push | cap;
                break;
            }

            case 3: { // Knight
                targets = bb_knight[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 6: { // King
                targets = bb_king[sq] & ~self_occ;
                uint32_t opp_targets = targets & oppn_occ;
                while(opp_targets){
                    int to = __builtin_ctz(opp_targets);
                    opp_targets &= opp_targets - 1;
                    if(oppn_pt[to] == 6){
                        this->game_state = WIN;
                        this->legal_actions.push_back(
                            Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
                        return;
                    }
                }
                break;
            }

            case 2: // Rook
            case 4: // Bishop
            case 5: { // Queen
                int d_start = (piece == 4) ? 4 : 0;
                int d_end   = (piece == 2) ? 4 : 8;
                for(int d = d_start; d < d_end; d++){
                    int cr = r + bb_dr[d], cc = c + bb_dc[d];
                    while(cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W){
                        int to = BB_SQ(cr, cc);
                        uint32_t to_bit = 1u << to;
                        if(self_occ & to_bit){
                            break; // own piece blocks
                        }

                        if((oppn_occ & to_bit) && oppn_pt[to] == 6){
                            this->game_state = WIN;
                            this->legal_actions.push_back(
                                Move(Point(r, c), Point(cr, cc)));
                            return;
                        }

                        targets |= to_bit;
                        if(oppn_occ & to_bit){
                            break; // captured, stop sliding
                        }
                        cr += bb_dr[d]; cc += bb_dc[d];
                    }
                }
                break;
            }
        }

        // Convert target bitmask to Move objects
        while(targets){
            int to = __builtin_ctz(targets);
            targets &= targets - 1;
            this->legal_actions.push_back(
                Move(Point(r, c), Point(BB_ROW(to), BB_COL(to))));
        }
    }
}


/*============================================================
 * Dispatcher
 *============================================================*/
void State::get_legal_actions(){
    #ifdef USE_BITBOARD
    get_legal_actions_bitboard();
    #else
    get_legal_actions_naive();
    #endif
}


const char piece_table[2][7][5] = {
  {" ", "♙", "♖", "♘", "♗", "♕", "♔"},
  {" ", "♟", "♜", "♞", "♝", "♛", "♚"}
};
/**
 * @brief encode the output for command line output
 * 
 * @return std::string 
 */
std::string State::encode_output() const{
    std::stringstream ss;
    int now_piece;
    for(int i=0; i<BOARD_H; i+=1){
        for(int j=0; j<BOARD_W; j+=1){
            if((now_piece = this->board.board[0][i][j])){
                ss << std::string(piece_table[0][now_piece]);
            }else if((now_piece = this->board.board[1][i][j])){
                ss << std::string(piece_table[1][now_piece]);
            }else{
                ss << " ";
            }
            ss << " ";
        }
        ss << "\n";
    }
    return ss.str();
}


/**
 * @brief encode the state to the format for player
 * 
 * @return std::string 
 */
std::string State::encode_state(){
    std::stringstream ss;
    ss << this->player;
    ss << "\n";
    for(int pl=0; pl<2; pl+=1){
        for(int i=0; i<BOARD_H; i+=1){
            for(int j=0; j<BOARD_W; j+=1){
                ss << int(this->board.board[pl][i][j]);
                ss << " ";
            }
            ss << "\n";
        }
        ss << "\n";
    }
    return ss.str();
}


BaseState* State::create_null_state() const{
    State* s = new State(this->board, 1 - this->player);
    s->get_legal_actions();
    return s;
}


/* === Board serialization === */
static const char* piece_chars = ".PRNBQK";
static const char* piece_chars_lower = ".prnbqk";

std::string State::encode_board() const{
    std::string s;
    for(int r = 0; r < BOARD_H; r++){
        if(r > 0){
            s += '/';
        }
        for(int c = 0; c < BOARD_W; c++){
            int w = board.board[0][r][c];
            int b = board.board[1][r][c];
            if(w > 0 && w <= 6){
                s += piece_chars[w];
            }else if(b > 0 && b <= 6){
                s += piece_chars_lower[b];
            }else{
                s += '.';
            }
        }
    }
    return s;
}

void State::decode_board(const std::string& s, int side_to_move){
    player = side_to_move;
    game_state = UNKNOWN;
    zobrist_valid = false;
    board = Board{};
    int r = 0, c = 0;
    for(char ch : s){
        if(ch == '/'){
            r++;
            c = 0;
            continue;
        }
        if(r >= BOARD_H || c >= BOARD_W){
            break;
        }
        if(ch >= 'A' && ch <= 'Z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars[p] == ch){
                    board.board[0][r][c] = p;
                    break;
                }
            }
        }else if(ch >= 'a' && ch <= 'z'){
            for(int p = 1; p <= 6; p++){
                if(piece_chars_lower[p] == ch){
                    board.board[1][r][c] = p;
                    break;
                }
            }
        }
        c++;
    }
    get_legal_actions();
}


/* (Zobrist tables moved above next_state) */


/*============================================================
 * Cell display for protocol (d command)
 *============================================================*/
std::string State::cell_display(int row, int col) const{
    int w = static_cast<int>(board.board[0][row][col]);
    int b = static_cast<int>(board.board[1][row][col]);
    if(w){
        const char* names = ".PRNBQK";
        return std::string(" ") + names[w] + " ";
    }else if(b){
        const char* names = ".prnbqk";
        return std::string(" ") + names[b] + " ";
    }else{
        return " . ";
    }
}

/* === Repetition: chess 3-fold rule === */
bool State::check_repetition(const GameHistory& history, int& out_score) const {
    if(history.count(hash()) >= 3){
        out_score = 0;  /* draw */
        return true;
    }
    return false;
}
