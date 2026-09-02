#ifndef _TOURNAMENT_H_
#define _TOURNAMENT_H_

#include <vector>
#include <memory>
#include <string>
#include <map>

#include "Player.h"


class RankingLog {
public:
    int player_id;
    int score;
    float bh_c1;
    float sb;
    float aob;
};


/**
 * A wrapper to handle a full blown swiss tournament.
 * Uses the bbpPairing pairing engine. 
 */
class Tournament{
private:
    void create_initial_ordering();

public:
    int round;
    int max_rounds;
    bool first_table_white = true;
    bool tournament_started;
    std::vector<Player> player_list;

    // std::vector<int> rankings_ids;
    std::vector<std::vector<RankingLog>> ranking_history;
    std::vector<std::vector<Match>> pairing_history;
    std::map<int, int> player_id_to_idx;


    std::string tournament_name;
    std::string tournament_city;
    std::string federation;
    std::string chief_arbiter;

    Tournament();
    Tournament(std::string tournament_name, std::string tournament_city, 
            std::string federation, std::string chief_arbiter, int rounds);

    // Utility methods for manipulating players & player information.
    void add_player(std::string name, int rating);
    void remove_player(std::string name);
    void remove_player_idx(int idx);
    void deactivate_player(std::string name);
    void deactivate_player_idx(int idx);
    void activate_player(std::string name);
    void activate_player_idx(int idx);
    void change_player_rating(std::string name, int new_rating);
    void change_player_name(std::string old_name, std::string new_name);
    void change_player_rating_idx(int idx, int new_rating);
    void change_player_name_idx(int idx, std::string new_name);

    void calculate_tiebreak();

    // Initializes the tournament with the given player list.
    void start_tournament();
    
    // Pairing Functionality
    static Tournament read_trf_file(const std::string& path);
    bool create_trf_file(const std::string& path = "out.trf");
    void create_pairing();
    void delete_current_pairing();
    void enter_pairing_result(int idx, MatchResult res);
    void generate_ranking();
    void remove_last_ranking();
};

#endif
