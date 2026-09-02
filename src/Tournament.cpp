#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <numeric>
#include <cstdlib>

#include "../include/Tournament.h"
#include "../include/Match.h"

Tournament::Tournament() : round(0), max_rounds(0), tournament_started(false) {

}

Tournament::Tournament(std::string tournament_name, std::string tournament_city, 
                        std::string federation, std::string chief_arbiter, int rounds) 
    : round(0), max_rounds(rounds), tournament_started(false), tournament_name(tournament_name),
    tournament_city(tournament_city), federation(federation), chief_arbiter(chief_arbiter) {

}

void Tournament::add_player(std::string name, int rating){
    auto it = std::find_if(player_list.begin(), player_list.end(), [name](Player& player){
        return player.name == name;
    });
    if(it != player_list.end())
        return;
    Player p(name, rating);
    for(int i = 0; i < round; i++){
        p.player_matches.push_back(Match(i+1, p.id, p.points, MatchResult::UNMATCHED));
    }
    player_list.push_back(p);

    if(tournament_started){
        create_initial_ordering();
    }
}

void Tournament::remove_player(std::string name){
    auto it = std::find_if(player_list.begin(), player_list.end(), [name](Player& player){
        return player.name == name;
    });
    player_list.erase(it);
}

void Tournament::remove_player_idx(int idx){
    player_list.erase(player_list.begin() + idx);
}

void Tournament::deactivate_player(std::string name){
    auto it = std::find_if(player_list.begin(), player_list.end(), [name](Player& player){
        return player.name == name;
    });
    it->active = false;
}

void Tournament::deactivate_player_idx(int idx){
    player_list[idx].active = false;
}

void Tournament::activate_player(std::string name){
    auto it = std::find_if(player_list.begin(), player_list.end(), [name](Player& player){
        return player.name == name;
    });
    it->active = true;
}

void Tournament::activate_player_idx(int idx){
    player_list[idx].active = true;
}

void Tournament::change_player_rating(std::string name, int new_rating){
    auto it = std::find_if(player_list.begin(), player_list.end(), [name](Player& player){
        return player.name == name;
    });
    it->rating = new_rating;
    if(tournament_started)
        create_initial_ordering();
}

void Tournament::change_player_rating_idx(int idx, int new_rating){
    player_list[idx].rating = new_rating;
    if(tournament_started)
        create_initial_ordering();
}

void Tournament::change_player_name(std::string name, std::string new_name){
    auto it = std::find_if(player_list.begin(), player_list.end(), [name](Player& player){
        return player.name == name;
    });
    it->name = new_name;
}

void Tournament::change_player_name_idx(int idx, std::string new_name){
    player_list[idx].name = new_name;
}


void Tournament::calculate_tiebreak(){
    for (Player& p : player_list) {
        p.bh_c1 = 0.0, p.sb = 0.0, p.aob = 0.0;
        int min = 999;
        for (Match& m : p.player_matches) {
            int opponent_id = m.get_opponent_id(p.id);
            if(opponent_id == -1){
                min = 0;
                continue;
            }
            int opponent_idx = this->player_id_to_idx.at(opponent_id);
            Player opponent = this->player_list[opponent_idx];
            if(opponent.points < min)
                min = opponent.points;
            p.bh_c1 += opponent.points;
            p.sb += opponent.points * player_result_to_points.at(m.get_player_result(p.id));
        }

        if (min != 999)
            p.bh_c1 -= min;

        p.bh_c1 /= 2; p.sb /= 4;
    }

    for (Player& p : player_list) {
        for (Match& m : p.player_matches) {
            int opponent_id = m.get_opponent_id(p.id);
            if(opponent_id == -1)
                continue;
            int opponent_idx = this->player_id_to_idx.at(opponent_id);
            Player opponent = this->player_list[opponent_idx];
            p.aob += opponent.bh_c1;
        }
        p.aob /= p.player_matches.size();
    }
}

// TODO: Add Titles
void Tournament::create_initial_ordering(){
    std::sort(player_list.begin(), player_list.end(), 
        [](Player& p1, Player& p2){
            if(p1.rating != p2.rating)
                return p1.rating > p2.rating;
            return p1.name < p2.name;
        }
    );

    player_id_to_idx.clear();
    for(int i = 0; i < (int)player_list.size(); i++){
        player_id_to_idx.insert(std::make_pair(player_list[i].id, i));
    }
}

// TODO: This method cannot parse accurate scores in pairing history.
// Only the final scores of players are recorded.
Tournament Tournament::read_trf_file(const std::string& path){
    // Tokenize the input file
    std::ifstream trf_file(path);
    std::vector<std::string> file_lines;
    std::string file_line;
    while(std::getline(trf_file, file_line)){
        file_lines.push_back(file_line);
    }
    Tournament t;

    // Interpret.
    for(const std::string& line: file_lines) {
        // Every DIN line we parse is at least 5 chars ("XXR 9"); shorter
        // lines are blank or truncated, and substr(4) would throw on them.
        if(line.size() < 5)
            continue;

        std::string data_identification_number = line.substr(0, 3); 
        if(data_identification_number == "XXC") {
            t.first_table_white = (line.substr(4) == "white1");
        }
        else if(data_identification_number == "XXR") {
            t.max_rounds = std::stoi(line.substr(4));
        }
        else if(data_identification_number == "001") {
            // Only the initial configuration is restored: name and rating.
            // Points and per-round results are deliberately discarded, so a
            // file saved mid-tournament loads back as a fresh player list.
            // Note the writer still emits them, since create_pairing feeds the
            // same format to bbpPairings, which needs the round history.
            // The name column is space-padded to a fixed width; trim it, or
            // every loaded player keeps trailing blanks in their name.
            std::string name = line.substr(14, 32);
            size_t name_end = name.find_last_not_of(' ');
            name = (name_end == std::string::npos) ? "" : name.substr(0, name_end + 1);
            // Throws on a short or non-numeric line, which is how a non-TRF
            // file is rejected by the caller.
            int fide_rating = std::stoi(line.substr(48, 4));

            // The file's starting rank is not reused as the player id: ids come
            // from Player's counter, so they cannot collide with players added
            // after the load.
            t.player_list.push_back(Player(name, fide_rating));
        }
        else if(data_identification_number == "012") {
            t.tournament_name = line.substr(4);
        }
        else if(data_identification_number == "022") {
            t.tournament_city = line.substr(4);
        }
        else if(data_identification_number == "032") {
            t.federation = line.substr(4);
        }
        else if(data_identification_number == "042") {
            // date of start
        }
        else if(data_identification_number == "052") {
            // date of end
        }
        else if(data_identification_number == "062") {
            // number of players
        }
        else if(data_identification_number == "072") {
            // number of rated players
        }
        else if(data_identification_number == "082") {
            // number of teams
        }
        else if(data_identification_number == "092") {
            // type of tournament
        }
        else if(data_identification_number == "102") {
            t.chief_arbiter = line.substr(4);
        }
        else if(data_identification_number == "112") {
            // deputy chief arbiter (one line for each arbiter)
        }
        else if(data_identification_number == "122") {
            // allotted times per moves/game
        }
        else if(data_identification_number == "132") {
            // dates of the round  YY/MM/DD
        }
    }

    // round stays 0: a loaded file is always an unstarted tournament, so there
    // is nothing to start here.
    return t;
}

void Tournament::start_tournament(){
    if(tournament_started)
        return;
    tournament_started = true;
    create_initial_ordering();
}

bool Tournament::create_trf_file(const std::string& path){
    std::ofstream output_trf(path);
    if(!output_trf.is_open())
        return false;

    // DIN + space + value; read_trf_file reads the value as line.substr(4).
    // Empty fields are skipped: they would emit a 4-char line, and the reader
    // only accepts lines of length 5 or more.
    if(!tournament_name.empty()) output_trf << "012 " << tournament_name << "\n";
    if(!tournament_city.empty()) output_trf << "022 " << tournament_city << "\n";
    if(!federation.empty())      output_trf << "032 " << federation      << "\n";
    if(!chief_arbiter.empty())   output_trf << "102 " << chief_arbiter   << "\n";

    output_trf << "XXC " << (first_table_white ? "white1" : "black1") << "\n";
    output_trf << "XXR " << max_rounds << "\n";

    int idx = 1;
    for(const Player& player : player_list){
        output_trf << "001 ";
        output_trf << std::right << std::setw(4) << idx << " ";
        output_trf << "m";
        output_trf << std::left << std::setw(3) << "   " << " "; // FOR FIDE TITLE
        output_trf << std::left << std::setw(33) << player.name.substr(0, 32) << " ";
        output_trf << std::left << std::setw(4) << player.rating << " ";
        output_trf << std::left << std::setw(3) << "TUR" << " "; // FOR FIDE FEDERATION
        output_trf << std::right << std::setw(11) << "00000000" << " "; // FIDE ID
        output_trf << std::left << std::setw(10) << "2024" << " "; // BIRTH DATE (YYYY/MM/DD)
        output_trf << std::right << std::setw(4) << std::fixed << std::setprecision(1) << ((float)player.points/2.) << " "; // POINTS
        output_trf << std::left << std::setw(4) << " " << "  "; // RANK

        for(const Match& match : player.player_matches){
            std::string opponent_string;
            if(match.match_no_opponent())
                opponent_string = "    ";
            else{
                int opponent_id = match.get_opponent_id(player.id);
                int opponent_idx = player_id_to_idx[opponent_id];
                opponent_string = std::to_string(++opponent_idx);
            }
            output_trf << std::right << std::setw(4) << opponent_string << " ";

            std::string color_string;
            int color = match.get_player_color(player.id);
            if(color == -1) color_string = "-";
            else if(color == 0) color_string = "w";
            else color_string = "b";
            output_trf << color_string << " ";

            std::string result_string;
            PlayerResult player_result = match.get_player_result(player.id);
            result_string = result_to_rtfchar.at(player_result);
            output_trf << result_string << "  ";
        }

        if(!player.active)
            output_trf << "     - Z  ";

        idx++;
        output_trf << "\n";
    }

    output_trf.close();
    return output_trf.good();
}

bool Tournament::create_pairing(){
    round++;
    create_trf_file();
    std::string command("./bin/bbpPairings.exe --dutch out.trf -p round.txt");
    int result = system(command.c_str());
    if(result != 0){
        std::cerr << "Error: could not run the pairing engine.\n"
                  << "  command: " << command << "\n"
                  << "  status:  " << result << "\n";
        // Returning rather than exiting, so an ongoing tournament is not lost.
        // The round increment above has to be undone, or round and
        // pairing_history desync and the next Finalize scores the wrong round.
        round--;
        return false;
    }
    
    std::vector<Match> cur_pairing;
    std::ifstream pairing_stream("round.txt");
    int number_of_pairs = 0;
    pairing_stream >> number_of_pairs;
    for(int i = 0; i < number_of_pairs; i++){
        int first_player_idx, second_player_idx;
        pairing_stream >> first_player_idx >> second_player_idx;
        if(second_player_idx == 0){ // BYE CONDITION
            int first_player_id = player_list[first_player_idx-1].id;
            cur_pairing.push_back(
                Match(round, first_player_id, player_list[first_player_idx-1].points,  
                    MatchResult::PAIRING_ALLOCATED_BYE)
            );
            continue;
        }
        first_player_idx--; second_player_idx--;
        int first_player_id = player_list[first_player_idx].id;
        int second_player_id = player_list[second_player_idx].id;
        cur_pairing.push_back(
            Match(round, first_player_id, second_player_id, 
                player_list[first_player_idx].points, player_list[second_player_idx].points,
                MatchResult::UNINITIALIZED
            )
        );
    }
    pairing_history.push_back(cur_pairing);
    return true;
}

void Tournament::delete_current_pairing(){
    round--;
    pairing_history.pop_back();
}

void Tournament::enter_pairing_result(int idx, MatchResult res){
    pairing_history.back()[idx].game_result = res;
}

void Tournament::generate_ranking(){
    std::vector<int> current_ranking(player_list.size());
    std::vector<RankingLog> current_ranking_log(player_list.size());
    std::iota(current_ranking.begin(), current_ranking.end(), 0);

    std::sort(current_ranking.begin(), current_ranking.end(), [this](const int& p1, const int& p2){
        if(this->player_list[p1].points != this->player_list[p2].points)
            return this->player_list[p1].points > this->player_list[p2].points;
        if(this->player_list[p1].bh_c1 != this->player_list[p2].bh_c1)
            return this->player_list[p1].bh_c1 > this->player_list[p2].bh_c1;
        if(this->player_list[p1].sb != this->player_list[p2].sb)
            return this->player_list[p1].sb > this->player_list[p2].sb;
        if(this->player_list[p1].aob != this->player_list[p2].aob)
            return this->player_list[p1].aob > this->player_list[p2].aob;
        return p1 < p2;
    });

    for(int i = 0; i < (int)current_ranking.size(); i++){
        RankingLog r;
        r.player_id = player_list[current_ranking[i]].id;
        r.score = player_list[current_ranking[i]].points;
        r.bh_c1 = player_list[current_ranking[i]].bh_c1;
        r.sb = player_list[current_ranking[i]].sb;
        r.aob = player_list[current_ranking[i]].aob;
        current_ranking_log[i] = r;
    }
    ranking_history.push_back(current_ranking_log);
}

void Tournament::remove_last_ranking(){
    ranking_history.pop_back();
}
