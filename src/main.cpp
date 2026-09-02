#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "ImGuiFileDialog.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <iostream>
#include <cstring>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <cmath>

#include "Tournament.h"

// Which listing tab is on screen, and which one "Save PNG" exports.
enum ListingPage{
    LISTING_INITIAL = 0,
    LISTING_PAIRINGS = 1,
    LISTING_RANKINGS = 2
};

// Store State Variables
struct StateVariables{
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool show_create_tournament = false;
    bool show_add_player = false;
    bool show_modify_player = false;
    bool tournament_loaded = false;
    bool tournament_started = false;
    bool listings_hovered = false;
    bool pairing_online = false;
    int player_selected_idx = -1;
    int pairing_selected_idx = -1;

    int UI_round = 0;

    bool show_error = false;
    std::string error_message;
    std::string message_title = "Error";

    int active_listing = LISTING_INITIAL;

    // PNG export runs over several frames: the table needs three to settle its
    // column widths, then the fourth captures.
    bool export_pending = false;
    int export_frames = 0;
    float export_w = 0.0f;
    float export_h = 0.0f;
};

// Defined further down, but needed by the file helpers above them.
void show_message(StateVariables& sv, const char* title, const std::string& text);
const char* listing_slug(int page);

int initialize(SDL_Renderer*& renderer, SDL_Window*& window, ImGuiIO& io){
    // Setup SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // From 2.0.18: Enable native IME.
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif


    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    window = SDL_CreateWindow("Bilkent Swiss", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
    if (window == nullptr)
    {
        printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return -1;
    }
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr)
    {
        SDL_Log("Error creating SDL_Renderer!");
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    return 0;
}

Tournament load_tournament(char tournament_name[], char city[],
                            char federation[], char chief_arbiter[], int rounds){
    Tournament t(tournament_name, city, federation, chief_arbiter, rounds);
    return t;
}

bool load_trf_file(Tournament& t, StateVariables& sv, const std::string& trf_path){
    std::ifstream probe(trf_path);
    if(!probe.is_open()){
        show_message(sv, "Error", "Could not open file:\n" + trf_path);
        return false;
    }
    probe.close();

    // read_trf_file indexes fixed columns and calls stoi/stof without
    // validating them, so anything that is not a TRF file throws. Parse into a
    // temporary first, so a bad file cannot destroy the loaded tournament.
    Tournament candidate;
    try {
        candidate = Tournament::read_trf_file(trf_path);
    }
    catch(const std::exception& e){
        show_message(sv, "Error", "Not a valid TRF file:\n" + trf_path + "\n\n" + e.what());
        return false;
    }
    if(candidate.player_list.empty() && candidate.tournament_name.empty()){
        show_message(sv, "Error", "Not a valid TRF file:\n" + trf_path);
        return false;
    }

    t = candidate;
    sv.player_selected_idx = -1;
    sv.pairing_selected_idx = -1;
    sv.tournament_loaded = true;
    sv.tournament_started = (t.round > 0);
    sv.UI_round = 0;
    sv.pairing_online = false;
    return true;
}

void save_trf_file(Tournament& t, StateVariables& sv, const std::string& trf_path){
    if(!t.create_trf_file(trf_path)){
        show_message(sv, "Error", "Could not write tournament to:\n" + trf_path);
    }
}

// Turns a tournament name into something safe to put in a filename.
std::string sanitize_name(const std::string& in){
    std::string name = in;
    for(char& c : name){
        if(!std::isalnum((unsigned char)c) && c != '-' && c != '_')
            c = '_';
    }
    if(name.empty())
        name = "tournament";
    return name;
}

std::string default_trf_filename(const Tournament& t){
    return sanitize_name(t.tournament_name) + ".trf";
}

// Saved tournaments live here. The directory is gitignored, so a fresh clone
// will not have it; create it on demand instead of opening the dialog on a
// path that does not exist.
static const char* CONFIG_DIR = "configs";

void ensure_config_dir(){
    std::error_code ec;
    std::filesystem::create_directories(CONFIG_DIR, ec);
}

// Exported PNGs live here. Also gitignored, so also created on demand.
static const char* EXPORT_DIR = "exports";

// The export window is laid out far larger than any real table and the
// resulting image is cropped to the measured content. An explicit size is
// required because ImGui clamps auto-resized windows to the viewport.
static const float EXPORT_LAYOUT_W = 4000.0f;
static const float EXPORT_LAYOUT_H = 60000.0f;

void ensure_export_dir(){
    std::error_code ec;
    std::filesystem::create_directories(EXPORT_DIR, ec);
}

// <name>_round<N>_<page>.png, with _2 / _3 ... so repeat exports of the same
// round never overwrite an earlier one.
std::string unique_export_path(const Tournament& t, const StateVariables& sv){
    std::string base = sanitize_name(t.tournament_name);
    if(sv.active_listing != LISTING_INITIAL)
        base += "_round" + std::to_string(sv.UI_round);
    base += std::string("_") + listing_slug(sv.active_listing);

    std::filesystem::path dir(EXPORT_DIR);
    std::filesystem::path p = dir / (base + ".png");
    for(int n = 2; std::filesystem::exists(p); n++)
        p = dir / (base + "_" + std::to_string(n) + ".png");
    return p.string();
}

// Renders one ImGui draw list into an off-screen texture and writes it out as a
// PNG. The draw list must have been laid out at full size already; see the
// export window in main() for how that is arranged.
bool export_drawlist_to_png(SDL_Renderer* renderer, ImDrawList* draw_list, int w, int h,
                            const ImVec4& bg, const std::string& path, std::string& err){
    if(draw_list == nullptr || w <= 0 || h <= 0){
        err = "There is nothing to export.";
        return false;
    }
    if(!SDL_RenderTargetSupported(renderer)){
        err = "This renderer cannot draw to a texture, so exporting is unavailable.";
        return false;
    }

    SDL_RendererInfo info;
    if(SDL_GetRendererInfo(renderer, &info) == 0){
        if((info.max_texture_width > 0 && w > info.max_texture_width) ||
           (info.max_texture_height > 0 && h > info.max_texture_height)){
            err = "The table is too large to export (" + std::to_string(w) + "x"
                + std::to_string(h) + " pixels, limit is " + std::to_string(info.max_texture_width)
                + "x" + std::to_string(info.max_texture_height) + ").";
            return false;
        }
    }

    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_TARGET, w, h);
    if(target == nullptr){
        err = std::string("SDL_CreateTexture: ") + SDL_GetError();
        return false;
    }

    // The ImGui backend restores only the viewport and clip rect, so the render
    // target, scale, draw colour and blend mode are ours to put back. The scale
    // in particular is left at the display scale by the main loop, and the
    // export must render 1:1.
    SDL_Texture* old_target = SDL_GetRenderTarget(renderer);
    float old_scale_x = 1.0f, old_scale_y = 1.0f;
    SDL_RenderGetScale(renderer, &old_scale_x, &old_scale_y);
    Uint8 old_r, old_g, old_b, old_a;
    SDL_GetRenderDrawColor(renderer, &old_r, &old_g, &old_b, &old_a);
    SDL_BlendMode old_blend;
    SDL_GetRenderDrawBlendMode(renderer, &old_blend);

    SDL_SetRenderTarget(renderer, target);
    SDL_RenderSetScale(renderer, 1.0f, 1.0f);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    // The export window draws no background of its own, so paint one here.
    SDL_SetRenderDrawColor(renderer, (Uint8)(bg.x * 255), (Uint8)(bg.y * 255),
                                     (Uint8)(bg.z * 255), 255);
    SDL_RenderClear(renderer);

    // A hand-built draw data holding just this one list. Filled in directly
    // rather than via AddDrawList(), which would re-run bookkeeping we do not
    // want on a list ImGui still owns.
    ImDrawData draw_data;
    draw_data.Valid = true;
    draw_data.DisplayPos = ImVec2(0.0f, 0.0f);
    draw_data.DisplaySize = ImVec2((float)w, (float)h);
    draw_data.FramebufferScale = ImVec2(1.0f, 1.0f);
    draw_data.CmdLists.push_back(draw_list);
    draw_data.CmdListsCount = 1;
    draw_data.TotalVtxCount = draw_list->VtxBuffer.Size;
    draw_data.TotalIdxCount = draw_list->IdxBuffer.Size;
    ImGui_ImplSDLRenderer2_RenderDrawData(&draw_data, renderer);

    // SDL_RenderReadPixels reads through the viewport, which the backend left
    // set to the last draw command.
    SDL_RenderSetViewport(renderer, nullptr);
    SDL_RenderSetClipRect(renderer, nullptr);

    bool ok = false;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if(surface == nullptr)
        err = std::string("SDL_CreateRGBSurfaceWithFormat: ") + SDL_GetError();
    else if(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                 surface->pixels, surface->pitch) != 0)
        err = std::string("SDL_RenderReadPixels: ") + SDL_GetError();
    else if(IMG_SavePNG(surface, path.c_str()) != 0)
        err = std::string("IMG_SavePNG: ") + IMG_GetError();
    else
        ok = true;

    if(surface != nullptr)
        SDL_FreeSurface(surface);
    SDL_SetRenderTarget(renderer, old_target);
    SDL_RenderSetScale(renderer, old_scale_x, old_scale_y);
    SDL_SetRenderDrawColor(renderer, old_r, old_g, old_b, old_a);
    SDL_SetRenderDrawBlendMode(renderer, old_blend);
    SDL_DestroyTexture(target);
    return ok;
}

// Single entry points, shared by the File menu items and the Edit Tournament buttons.
void open_load_dialog(){
    ensure_config_dir();
    IGFD::FileDialogConfig config;
    config.path = CONFIG_DIR;
    config.countSelectionMax = 1;
    ImGuiFileDialog::Instance()->OpenDialog("LoadTRF", "Load Tournament", ".trf", config);
}

void open_save_dialog(const Tournament& t){
    ensure_config_dir();
    IGFD::FileDialogConfig config;
    config.path = CONFIG_DIR;
    config.fileName = default_trf_filename(t);
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_ConfirmOverwrite;
    ImGuiFileDialog::Instance()->OpenDialog("SaveTRF", "Save Tournament", ".trf", config);
}

// One small window used for both failures and the export confirmation. The
// title is part of the window id, hence NoSavedSettings.
void show_message(StateVariables& sv, const char* title, const std::string& text){
    sv.message_title = title;
    sv.error_message = text;
    sv.show_error = true;
}

void show_message_window(StateVariables& sv){
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x / 3, 0));
    ImGui::Begin(sv.message_title.c_str(), &sv.show_error,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextWrapped("%s", sv.error_message.c_str());
    ImGui::Separator();
    if(ImGui::Button("OK", ImVec2(-FLT_MIN, 0)))
        sv.show_error = false;
    ImGui::End();
}

void show_create_tournament_window(Tournament& t, StateVariables& sv){
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x / 3, 181));

    static char tournament_name[128] = "";
    static char city[128] = "";
    static char federation[128] = "";
    static char chief_arbiter[128] = "";
    static int rounds = 9;
    ImGui::Begin("Create Tournament", &sv.show_create_tournament, ImGuiWindowFlags_NoResize);
    if(ImGui::BeginTable("Tournament Info", 2, ImGuiTableFlags_SizingFixedFit)){
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch);

        // TOURNAMENT NAME
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Tournament Name");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputText("Tournament Name", tournament_name, 128);
        ImGui::PopItemWidth();

        // TOURNAMENT CITY
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("City");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputText("City", city, 128);
        ImGui::PopItemWidth();

        // TOURNAMENT FEDERATION
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Federation");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputText("Federation", federation, 128);
        ImGui::PopItemWidth();

        // CHIEF ARBITER
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Chief Arbiter");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputText("Chief Arbiter", chief_arbiter, 128);
        ImGui::PopItemWidth();

        // Round Count
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Round Count");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputInt("Round Count", &rounds);
        ImGui::PopItemWidth();

        ImGui::EndTable();
    }

    ImGui::Separator();

    if(ImGui::BeginTable("Create Tournament Create/Cancel", 2, ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        if(ImGui::Button("Create", ImVec2(-FLT_MIN, 0))){
            t = load_tournament(tournament_name, city, federation, chief_arbiter, rounds);
            sv.tournament_loaded = true;
            sv.tournament_started = false;
            sv.show_create_tournament = false;
        }
        ImGui::TableSetColumnIndex(1);
        if(ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))){
            sv.show_create_tournament = false;
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void show_add_player_window(Tournament& t, StateVariables& sv){
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x / 3, 112));

    static char player_name[128];
    static int player_rating;
    ImGui::Begin("Add Player", &sv.show_add_player, ImGuiWindowFlags_NoResize);
    if(ImGui::BeginTable("Tournament Info", 2, ImGuiTableFlags_SizingFixedFit)){
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch);

        // PLAYER NAME
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Player Name");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputText("Player Name", player_name, 128);
        ImGui::PopItemWidth();

        // PLAYER RATING
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Player Rating");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputInt("Player Rating", &player_rating);
        ImGui::PopItemWidth();

        ImGui::EndTable();
    }

    ImGui::Separator();

    if(ImGui::BeginTable("Add Player Create/Cancel", 2, ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        if(ImGui::Button("Create", ImVec2(-FLT_MIN, 0))){
            t.add_player(player_name, player_rating);
            sv.show_add_player = false;
        }
        ImGui::TableSetColumnIndex(1);
        if(ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))){
            sv.show_add_player = false;
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

void show_modify_player_window(Tournament& t, StateVariables& sv){
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2), ImGuiCond_Appearing, ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x / 3, 112));

    static char player_name[128];
    static bool player_initialized = false;
    if(!player_initialized){
        std::strcpy(player_name, t.player_list[sv.player_selected_idx].name.c_str());
        player_initialized = true;
    }
    static int player_rating = t.player_list[sv.player_selected_idx].rating;
    ImGui::Begin("Edit Player", &sv.show_modify_player, ImGuiWindowFlags_NoResize);
    if(ImGui::BeginTable("Edit Player", 2, ImGuiTableFlags_SizingFixedFit)){
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch);

        // PLAYER NAME
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Player Name");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputText("Player Name", player_name, 128);
        ImGui::PopItemWidth();

        // PLAYER RATING
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::Text("Player Rating");

        ImGui::TableSetColumnIndex(1);
        ImGui::PushItemWidth(-1);
        ImGui::InputInt("Player Rating", &player_rating);
        ImGui::PopItemWidth();

        ImGui::EndTable();
    }

    ImGui::Separator();

    if(ImGui::BeginTable("Modify Player Create/Cancel", 2, ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        if(ImGui::Button("Edit", ImVec2(-FLT_MIN, 0))){
            t.change_player_name_idx(sv.player_selected_idx, player_name);
            t.change_player_rating_idx(sv.player_selected_idx, player_rating);
            sv.show_modify_player = false;
            player_initialized = false;
        }
        ImGui::TableSetColumnIndex(1);
        if(ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))){
            sv.show_modify_player = false;
            player_initialized = false;
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

// Draws text horizontally centered in the current window.
void draw_centered_text(const char* text){
    float windowWidth = ImGui::GetWindowSize().x;
    ImVec2 textSize = ImGui::CalcTextSize(text);
    float textX = (windowWidth - textSize.x) * 0.5f;
    if (textX > 0.0f) {
        ImGui::SetCursorPosX(textX);
    }
    ImGui::Text("%s", text);
}

// The listing tables are drawn in two modes. On screen (interactive) the name
// column stretches to fill the panel and rows are selectable. For the PNG
// export every column is fixed-width and the table does not extend to the host
// window, so it takes its natural width and the image can be cropped to it.
static ImGuiTableFlags listing_table_flags(bool interactive){
    ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit
        | ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
    if(!interactive)
        flags |= ImGuiTableFlags_NoHostExtendX;
    return flags;
}

static ImGuiTableColumnFlags listing_name_flags(bool interactive){
    return interactive ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed;
}

// Each draw_*_table returns whether any row was hovered (interactive only).
bool draw_initial_ranking_table(Tournament& tournament, StateVariables& sv, bool interactive){
    bool hovered = false;
    if(ImGui::BeginTable("Tournament Info", 5, listing_table_flags(interactive))){
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Name", listing_name_flags(interactive));
        ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Pairing Status", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Rating", ImGuiTableColumnFlags_WidthFixed);

        // Headers
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("SNo");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Name");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("Score");
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("Pairing Status");
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("Rtg");

        int idx = 1;
        for(Player& player : tournament.player_list){
            std::string idx_string = std::string(3 - std::to_string(idx).length(), ' ') + std::to_string(idx);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if(interactive){
                if(ImGui::Selectable(idx_string.c_str(), sv.player_selected_idx == idx-1,
                    ImGuiSelectableFlags_SpanAllColumns)){
                    if(sv.player_selected_idx == idx-1)
                        sv.player_selected_idx = -1;
                    else
                        sv.player_selected_idx = idx-1;
                }
                hovered |= ImGui::IsItemHovered();
            }
            else
                ImGui::TextUnformatted(idx_string.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", player.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%2.1f", player.points / 2.);
            if(!player.active){
                ImGui::TableSetColumnIndex(3);
                float column_width = ImGui::GetColumnWidth();
                ImVec2 text_size = ImGui::CalcTextSize("UNPAIRED");
                float text_x = (column_width - text_size.x) * 0.5f;
                if (text_x > 0.0f)
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + text_x);
                ImGui::Text("UNPAIRED");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%4d", player.rating);
            idx++;
        }
        ImGui::EndTable();
    }
    return hovered;
}

bool draw_pairing_table(Tournament& tournament, StateVariables& sv, bool interactive){
    bool hovered = false;
    if(ImGui::BeginTable("Tournament Pairings", 8, listing_table_flags(interactive))){
        ImGui::TableSetupColumn("Board1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("WhiteName1", listing_name_flags(interactive));
        ImGui::TableSetupColumn("WhiteRating1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("WhitePoint1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Result1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("BlackPoint1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("BlackName1", listing_name_flags(interactive));
        ImGui::TableSetupColumn("BlackRating1", ImGuiTableColumnFlags_WidthFixed);

        // Table Headers
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Bo.");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("White");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("Rtg");
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("Pts.");
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("  Result  ");
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("Pts.");
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("Black");
        ImGui::TableSetColumnIndex(7);
        ImGui::Text("Rtg");

        int idx = 1;
        for(Match match : tournament.pairing_history[sv.UI_round-1]){
            int first_player_idx = tournament.player_id_to_idx.at(match.white_player_id);
            int second_player_idx = match.black_player_id < 0 ? -1
                : tournament.player_id_to_idx.at(match.black_player_id);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%3d", idx);
            ImGui::TableSetColumnIndex(1);
            if(interactive){
                if(ImGui::Selectable(tournament.player_list[first_player_idx].name.c_str(), sv.pairing_selected_idx == idx-1,
                                    ImGuiSelectableFlags_SpanAllColumns)){
                    if(sv.pairing_selected_idx == idx-1)
                        sv.pairing_selected_idx = -1;
                    else
                        sv.pairing_selected_idx = idx-1;
                }
                hovered |= ImGui::IsItemHovered();
            }
            else
                ImGui::TextUnformatted(tournament.player_list[first_player_idx].name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%4d", tournament.player_list[first_player_idx].rating);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%2.1f", match.white_cur_score / 2.);

            ImGui::TableSetColumnIndex(4);
            float column_width = ImGui::GetColumnWidth();
            ImVec2 text_size = ImGui::CalcTextSize(result_to_string.at(tournament.pairing_history[sv.UI_round-1][idx-1].game_result).c_str());
            float text_x = (column_width - text_size.x) * 0.5f;
            if (text_x > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + text_x);
            ImGui::Text("%s", result_to_string.at(tournament.pairing_history[sv.UI_round-1][idx-1].game_result).c_str());

            ImGui::TableSetColumnIndex(5);
            if(second_player_idx < 0)
                ImGui::Text("  ");
            else
                ImGui::Text("%2.1f", match.black_cur_score / 2.);
            ImGui::TableSetColumnIndex(6);
            std::string black_name_field = "";
            if(second_player_idx < 0)
                black_name_field = "Bye";
            else{
                black_name_field = tournament.player_list[second_player_idx].name;
            }
            ImGui::Text("%s", black_name_field.c_str());
            ImGui::TableSetColumnIndex(7);
            if(second_player_idx < 0)
                ImGui::Text("    ");
            else
                ImGui::Text("%4d", tournament.player_list[second_player_idx].rating);
            idx++;
        }

        ImGui::EndTable();
    }
    return hovered;
}

bool draw_ranking_table(Tournament& tournament, StateVariables& sv, bool interactive){
    if(ImGui::BeginTable("Tournament Rankings", 7, listing_table_flags(interactive))){
        ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Name", listing_name_flags(interactive));
        ImGui::TableSetupColumn("Rating", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Points", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("BH-C1", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("SB", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("AOB", ImGuiTableColumnFlags_WidthFixed);

        // Headers
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("Rk.");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("Name");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("Rtg");
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("Pts");
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("BH-C1");
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("SB   ");
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("AOB  ");

        int idx = 1;
        for(RankingLog log : tournament.ranking_history[sv.UI_round-1]){
            Player& player = tournament.player_list[tournament.player_id_to_idx.at(log.player_id)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%3d", idx);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", player.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%4d", player.rating);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%2.1f", log.score / 2.);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%2.1f", log.bh_c1);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%2.1f", log.sb);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%2.1f", log.aob);
            idx++;
        }
        ImGui::EndTable();
    }
    (void)interactive;
    return false;   // the rankings table has no selectable rows
}

const char* listing_title(int page){
    switch(page){
        case LISTING_PAIRINGS: return "Pairings";
        case LISTING_RANKINGS: return "Rankings";
        default:               return "Initial Rankings";
    }
}

const char* listing_slug(int page){
    switch(page){
        case LISTING_PAIRINGS: return "pairings";
        case LISTING_RANKINGS: return "rankings";
        default:               return "initial";
    }
}

// Whether a page has something to show. These mirror the gates the tabs use, so
// the Save PNG button is only enabled when there is actually a table to export.
bool listing_has_data(const Tournament& tournament, const StateVariables& sv, int page){
    switch(page){
        case LISTING_PAIRINGS:
            return tournament.round > 0
                && sv.UI_round >= 1
                && (int)tournament.pairing_history.size() >= sv.UI_round;
        case LISTING_RANKINGS:
            // The UI_round >= 1 test matters: deleting a pairing can drive
            // UI_round to 0 while ranking_history is still populated.
            return sv.UI_round >= 1
                && (int)tournament.ranking_history.size() >= sv.UI_round
                && (int)tournament.ranking_history[sv.UI_round-1].size() > 0;
        default:
            return sv.tournament_loaded;
    }
}

bool draw_listing_table(Tournament& tournament, StateVariables& sv, int page, bool interactive){
    switch(page){
        case LISTING_PAIRINGS: return draw_pairing_table(tournament, sv, interactive);
        case LISTING_RANKINGS: return draw_ranking_table(tournament, sv, interactive);
        default:               return draw_initial_ranking_table(tournament, sv, interactive);
    }
}

void show_initial_ranking_listing(Tournament& tournament, StateVariables& sv){
    if (ImGui::BeginTabItem("Initial Rankings")){
        sv.active_listing = LISTING_INITIAL;
        if(listing_has_data(tournament, sv, LISTING_INITIAL)){
            draw_centered_text(tournament.tournament_name.c_str());
            bool hovered = draw_initial_ranking_table(tournament, sv, true);
            if (ImGui::IsMouseClicked(0) && !hovered && sv.listings_hovered) {
                sv.player_selected_idx = -1;  // Deselect if clicking outside the table
            }
        }
        ImGui::EndTabItem();
    }
}

void show_pairing_listing(Tournament& tournament, StateVariables& sv){
    if (ImGui::BeginTabItem("Pairings")){
        sv.active_listing = LISTING_PAIRINGS;
        // A loaded tournament has round > 0 but an empty pairing_history,
        // since read_trf_file does not rebuild it.
        if(listing_has_data(tournament, sv, LISTING_PAIRINGS)){
            draw_centered_text(tournament.tournament_name.c_str());
            bool hovered = draw_pairing_table(tournament, sv, true);
            if (ImGui::IsMouseClicked(0) && !hovered && sv.listings_hovered) {
                sv.pairing_selected_idx = -1;  // Deselect if clicking outside the table
            }
        }
        ImGui::EndTabItem();
    }
}

void show_ranking_listing(Tournament& tournament, StateVariables& sv){
    if (ImGui::BeginTabItem("Rankings")){
        sv.active_listing = LISTING_RANKINGS;
        if(listing_has_data(tournament, sv, LISTING_RANKINGS)){
            draw_centered_text(tournament.tournament_name.c_str());
            draw_ranking_table(tournament, sv, true);
        }
        ImGui::EndTabItem();
    }
}


// Lays the selected listing out at full size in an off-screen window and
// returns its draw list, recording the cropped image size in sv.export_w/h.
// Kept separate from the frame loop so it can be exercised without the UI.
ImDrawList* submit_export_window(Tournament& tournament, StateVariables& sv){
    ImDrawList* export_draw_list = nullptr;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(EXPORT_LAYOUT_W, EXPORT_LAYOUT_H));
        ImGui::Begin("##png_export", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
            | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();

        // Essential: Begin() clipped this window's rect to the display, so
        // without replacing the clip rect every row below the bottom of the
        // screen would be culled and the image would come out screen-tall.
        ImGui::PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(EXPORT_LAYOUT_W, EXPORT_LAYOUT_H), false);

        export_draw_list = ImGui::GetWindowDrawList();

        float header_right = 0.0f;
        float pad = ImGui::GetStyle().WindowPadding.x;
        {
            char subtitle[160];
            if(sv.active_listing == LISTING_INITIAL)
                snprintf(subtitle, sizeof(subtitle), "%s", listing_title(sv.active_listing));
            else
                snprintf(subtitle, sizeof(subtitle), "%s - Round %d/%d",
                    listing_title(sv.active_listing), sv.UI_round, tournament.max_rounds);

            // sv.export_w holds the previous export frame's width, so the
            // header is left aligned on the first frame and centered by the
            // time the capture frame runs.
            float center_w = sv.export_w;
            ImVec2 name_size = ImGui::CalcTextSize(tournament.tournament_name.c_str());
            ImVec2 sub_size = ImGui::CalcTextSize(subtitle);
            if(center_w > 0.0f){
                float x = (center_w - name_size.x) * 0.5f;
                if(x > pad) ImGui::SetCursorPosX(x);
            }
            ImGui::Text("%s", tournament.tournament_name.c_str());
            if(center_w > 0.0f){
                float x = (center_w - sub_size.x) * 0.5f;
                if(x > pad) ImGui::SetCursorPosX(x);
            }
            ImGui::Text("%s", subtitle);
            ImGui::Separator();
            header_right = pad + (name_size.x > sub_size.x ? name_size.x : sub_size.x);
        }

        ImGui::PushID(sv.active_listing);   // the three export tables share one window
        draw_listing_table(tournament, sv, sv.active_listing, false);
        ImGui::PopID();

        // EndTable() ends with ItemAdd(table->OuterRect), so this is the
        // exact table box; GetCursorPosY would add a stray ItemSpacing.
        ImVec2 table_max = ImGui::GetItemRectMax();
        sv.export_w = (table_max.x > header_right ? table_max.x : header_right) + pad;
        sv.export_h = table_max.y + ImGui::GetStyle().WindowPadding.y;

        ImGui::PopClipRect();
        ImGui::End();
    return export_draw_list;
}

int main(){
    SDL_Renderer* renderer; SDL_Window* window; ImGuiIO io;
    int err_code = initialize(renderer, window, io);
    if(err_code != 0)
        return -1;

    static Tournament tournament;
    static StateVariables sv;

    // Main loop
    bool done = false;
    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)){
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED){
            SDL_Delay(10);
            continue;
        }

        // Start the Dear ImGui frame
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        {
            io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("Bilkent Swiss", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoResize); // ImGuiWindowFlags_NoDecoration
            if (ImGui::BeginMenuBar()){ 
                if (ImGui::BeginMenu("File")){
                    // Load TRF file.
                    if(ImGui::MenuItem("Load Tournament", "Ctrl-O"))
                        open_load_dialog();
                    // Save as TRF.
                    if(ImGui::MenuItem("Save Tournament", "Ctrl-S", false, sv.tournament_loaded))
                        open_save_dialog(tournament);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            ImGui::BeginChild("Tournament", ImVec2(ImGui::GetContentRegionAvail().x * 0.7f, ImGui::GetContentRegionAvail().y),
                    ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            if(ImGui::BeginTabBar("Listings")){
                sv.listings_hovered = ImGui::IsWindowHovered();

                float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                float frame_h   = ImGui::GetFrameHeight();
                float save_w    = ImGui::CalcTextSize("Save PNG").x
                                + ImGui::GetStyle().FramePadding.x * 2.0f;

                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x 
                    - (save_w + spacing + ImGui::CalcTextSize("Round 99/99").x + 2*(frame_h+spacing)));

                // active_listing is set by whichever tab drew last frame, which
                // is what the export will use.
                bool save_png_disable_copy = sv.export_pending
                    || !listing_has_data(tournament, sv, sv.active_listing);
                if(save_png_disable_copy)
                    ImGui::BeginDisabled();
                if(ImGui::Button("Save PNG")){
                    sv.export_pending = true;
                    sv.export_frames = 4;   // three frames for the table to settle, then capture
                    sv.export_w = 0.0f;
                    sv.export_h = 0.0f;
                }
                if(save_png_disable_copy)
                    ImGui::EndDisabled();
                ImGui::SameLine();

                ImGui::Text("Round %d/%d", sv.UI_round, tournament.max_rounds);
                ImGui::SameLine();

                bool switch_lower_disable_copy = sv.UI_round <= 1;
                if(switch_lower_disable_copy)
                     ImGui::BeginDisabled(); 

                if (ImGui::ArrowButton("##left_round", ImGuiDir_Left)) {
                    if (sv.UI_round > 1)
                        sv.UI_round--;
                }
                if(switch_lower_disable_copy)
                     ImGui::EndDisabled(); 

                ImGui::SameLine();

                bool switch_upper_disable_copy = sv.UI_round >= tournament.round;
                if(switch_upper_disable_copy)
                     ImGui::BeginDisabled(); 

                if (ImGui::ArrowButton("##right_round", ImGuiDir_Right)) {
                    if (sv.UI_round < tournament.round)
                        sv.UI_round++;
                }
                if(switch_upper_disable_copy)
                     ImGui::EndDisabled(); 

                show_initial_ranking_listing(tournament, sv);
                show_pairing_listing(tournament, sv);
                show_ranking_listing(tournament, sv);

                ImGui::EndTabBar();
            }
            ImGui::EndChild();  

            ImGui::SameLine();

            ImGui::BeginChild("Manage", ImVec2(0, 0), ImGuiChildFlags_Borders);
            if(ImGui::BeginTabBar("Tournament Management")){
                if (ImGui::BeginTabItem("Edit Tournament")){

                    if(ImGui::Button("Create New Tournament", ImVec2(-FLT_MIN, 30))){
                        sv.show_create_tournament = true;
                    }

                    if(ImGui::Button("Load Tournament", ImVec2(-FLT_MIN, 30)))
                        open_load_dialog();

                    // Save stays available once a tournament is started, so it
                    // gets its own scope rather than joining the one below.
                    bool disable_save = !sv.tournament_loaded;
                    if(disable_save)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Save Tournament", ImVec2(-FLT_MIN, 30)))
                        open_save_dialog(tournament);
                    if(disable_save)
                        ImGui::EndDisabled();

                    if(!sv.tournament_loaded)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Add Player", ImVec2(-FLT_MIN, 30)))
                        sv.show_add_player = true;
                    if(sv.player_selected_idx == -1)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Edit Player", ImVec2(-FLT_MIN, 30)))
                        sv.show_modify_player = true;
                    if(sv.tournament_started)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Remove Player", ImVec2(-FLT_MIN, 30))){
                        tournament.remove_player_idx(sv.player_selected_idx);
                    }
                    if(sv.tournament_started)
                        ImGui::EndDisabled();
                    if(sv.player_selected_idx == -1)
                        ImGui::EndDisabled();

                    [](bool disable_this_button){
                        if(disable_this_button)
                            ImGui::BeginDisabled();
                        if(ImGui::Button("Start Tournament", ImVec2(-FLT_MIN, 30))){
                            tournament.start_tournament();
                            sv.tournament_started = true;
                        }
                        if(disable_this_button)
                            ImGui::EndDisabled();
                    }(sv.tournament_started);

                    if(!sv.tournament_loaded)
                        ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Manage Pairings")){
                    if(!sv.tournament_started)
                        ImGui::BeginDisabled();
                    
                    bool ponlinecopy = sv.pairing_online;
                    if(ponlinecopy)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Pair Next Round", ImVec2(-FLT_MIN, 30))){
                        if(!tournament.create_pairing()){
                            sv.error_message = "Could not run the pairing engine.\n\n"
                                "Make sure bin/bbpPairings.exe exists and that the "
                                "program is run from the project root.";
                            sv.show_error = true;
                        }
                        else{
                            sv.UI_round = tournament.round;
                            for(int i = 0; i < (int)tournament.player_list.size(); i++){
                                if(tournament.player_list[i].active)
                                    continue;
                                Match absent(tournament.round, tournament.player_list[i].id, 
                                    tournament.player_list[i].points, MatchResult::UNMATCHED
                                );
                                tournament.player_list[i].player_matches.push_back(absent);
                            }
                            sv.pairing_online = true;
                        }
                    }
                    if(ponlinecopy)
                        ImGui::EndDisabled();
                    
                    bool delete_current_copy = !ponlinecopy || tournament.round == 0;
                    if(delete_current_copy)
                        ImGui::BeginDisabled();

                    if(ImGui::Button("Delete Current Pairing", ImVec2(-FLT_MIN, 30))){
                        if(sv.UI_round == tournament.round)
                            sv.UI_round--;
                        tournament.delete_current_pairing();
                        for(int i = 0; i < (int)tournament.player_list.size(); i++){
                            if(tournament.player_list[i].active)
                                continue;
                            tournament.player_list[i].player_matches.pop_back();
                        }
                        sv.pairing_online = false;
                    }

                    if(delete_current_copy)
                        ImGui::EndDisabled();

                    if(!ponlinecopy)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Finalize Current Round", ImVec2(-FLT_MIN, 30))){
                        // TODO: Verify all results are entered.
                        for(int i = 0; i < (int)tournament.pairing_history.back().size(); i++){
                            std::pair<int, int> match_points = result_to_points.at(tournament.pairing_history.back()[i].game_result);
                            int white_idx = tournament.player_id_to_idx.at(tournament.pairing_history.back()[i].white_player_id);
                            if(tournament.pairing_history.back()[i].black_player_id < 0){          // BYEs
                                tournament.player_list[white_idx].points += match_points.first;
                                tournament.player_list[white_idx].player_matches.push_back(
                                    tournament.pairing_history.back()[i]
                                );
                                continue;
                            }
                            int black_idx = tournament.player_id_to_idx.at(
                                tournament.pairing_history.back()[i].black_player_id);
                            tournament.player_list[white_idx].player_matches.push_back(
                                tournament.pairing_history.back()[i]
                            );
                            tournament.player_list[black_idx].player_matches.push_back(
                                tournament.pairing_history.back()[i]
                            );

                            tournament.player_list[white_idx].points += match_points.first;
                            tournament.player_list[black_idx].points += match_points.second;
                        }
                        
                        tournament.calculate_tiebreak();
                        tournament.generate_ranking();
                        sv.pairing_online = false;
                    }
                    if(!ponlinecopy)
                        ImGui::EndDisabled();


                    if(ponlinecopy || tournament.round == 0)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Reopen Current Round", ImVec2(-FLT_MIN, 30))){
                        for(int i = 0; i < (int)tournament.pairing_history.back().size(); i++){
                            std::pair<int, int> match_points = result_to_points.at(tournament.pairing_history.back()[i].game_result);
                            int white_idx = tournament.player_id_to_idx.at(tournament.pairing_history.back()[i].white_player_id);
                            if(tournament.pairing_history.back()[i].black_player_id < 0){          // Undo BYEs
                                tournament.player_list[white_idx].points -= match_points.first;
                                tournament.player_list[white_idx].player_matches.pop_back();
                                continue;
                            }
                            int black_idx = tournament.player_id_to_idx.at(tournament.pairing_history.back()[i].black_player_id);
                            tournament.player_list[white_idx].player_matches.pop_back();
                            tournament.player_list[black_idx].player_matches.pop_back();

                            tournament.player_list[white_idx].points -= match_points.first;
                            tournament.player_list[black_idx].points -= match_points.second;
                        }
                        tournament.remove_last_ranking();
                        tournament.calculate_tiebreak();
                        sv.pairing_online = true;
                    }
                    if(ponlinecopy || tournament.round == 0)
                        ImGui::EndDisabled();
                    

                    int pidxcopy = sv.player_selected_idx;
                    bool pactivecopy = sv.player_selected_idx >= 0 ? tournament.player_list[sv.player_selected_idx].active : false;
                    if(pidxcopy == -1 
                        || pactivecopy)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Add Player to Pairings", ImVec2(-FLT_MIN, 30)))
                        tournament.activate_player_idx(sv.player_selected_idx);
                    if(pidxcopy == -1 
                        || pactivecopy)
                        ImGui::EndDisabled();
                    
                    if(pidxcopy == -1 || !pactivecopy)
                        ImGui::BeginDisabled();
                    if(ImGui::Button("Remove Player from Pairings", ImVec2(-FLT_MIN, 30)))
                        tournament.deactivate_player_idx(sv.player_selected_idx);
                    if(pidxcopy == -1 || !pactivecopy)
                        ImGui::EndDisabled();

                    ImGui::Separator();

                    if(sv.pairing_selected_idx == -1 || !sv.pairing_online || sv.UI_round != tournament.round)
                        ImGui::BeginDisabled();
                    if(ImGui::BeginTable("Tournament Info", 3, ImGuiTableFlags_SizingFixedFit)){
                        ImGui::TableSetupColumn("ROW1", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ROW2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ROW3", ImGuiTableColumnFlags_WidthStretch);

                        // Regular
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if(ImGui::Button("1 - 0", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::REGULAR_WHITE_WIN);
                        ImGui::TableNextColumn();
                        if(ImGui::Button("1/2 - 1/2", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::REGULAR_DRAW);
                        ImGui::TableNextColumn();
                        if(ImGui::Button("0 - 1", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::REGULAR_BLACK_WIN);

                        // Forfeit
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if(ImGui::Button("+ / -", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::FORFEIT_WHITE_WIN);
                        ImGui::TableNextColumn();
                        if(ImGui::Button("- / -", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::FORFEIT_BOTH);
                        ImGui::TableNextColumn();
                        if(ImGui::Button("- / +", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::FORFEIT_BLACK_WIN);

                        // Unrated
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if(ImGui::Button("W / L", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::UNRATED_WHITE_WIN);
                        ImGui::TableNextColumn();
                        if(ImGui::Button("D / D", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::UNRATED_DRAW);
                        ImGui::TableNextColumn();
                        if(ImGui::Button("L / W", ImVec2(-FLT_MIN, 30)))
                            tournament.enter_pairing_result(sv.pairing_selected_idx, MatchResult::UNRATED_BLACK_WIN);

                        ImGui::EndTable();
                    }
                    if(sv.pairing_selected_idx == -1 || !sv.pairing_online || sv.UI_round != tournament.round)
                        ImGui::EndDisabled();
                    if(!sv.tournament_started)
                        ImGui::EndDisabled();

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();  
            ImGui::End();
        }

        // Without a size constraint the dialog opens collapsed to its minimum
        // the first time, before imgui.ini has an entry for it.
        ImVec2 file_dialog_min(ImGui::GetIO().DisplaySize.x * 0.5f,
                               ImGui::GetIO().DisplaySize.y * 0.5f);

        if (ImGuiFileDialog::Instance()->Display("LoadTRF", ImGuiWindowFlags_NoCollapse, file_dialog_min)) {
            if (ImGuiFileDialog::Instance()->IsOk())
                load_trf_file(tournament, sv, ImGuiFileDialog::Instance()->GetFilePathName());
            ImGuiFileDialog::Instance()->Close();
        }

        if (ImGuiFileDialog::Instance()->Display("SaveTRF", ImGuiWindowFlags_NoCollapse, file_dialog_min)) {
            // GetFilePathName defaults to IGFD_ResultMode_AddIfNoFileExt, which
            // appends ".trf" when the typed name has no extension.
            if (ImGuiFileDialog::Instance()->IsOk())
                save_trf_file(tournament, sv, ImGuiFileDialog::Instance()->GetFilePathName());
            ImGuiFileDialog::Instance()->Close();
        }

        if(sv.show_error)
            show_message_window(sv);

        if(sv.show_create_tournament)
            show_create_tournament_window(tournament, sv);

        if(sv.show_add_player)
            show_add_player_window(tournament, sv);

        if(sv.show_modify_player)
            show_modify_player_window(tournament, sv);

        // ImGui::ShowDemoWindow();

        // The PNG export lays the selected listing out at full size in its own
        // window, submitted last so nothing can interleave with it. The window
        // is filtered out of the on-screen render below, so it is never seen.
        ImDrawList* export_draw_list = nullptr;
        bool export_capture = false;
        if(sv.export_pending){
            sv.export_frames--;
            export_capture = (sv.export_frames <= 0);

            export_draw_list = submit_export_window(tournament, sv);
        }

        // Rendering
        ImGui::Render();
        ImDrawData* frame_draw_data = ImGui::GetDrawData();

        // Everything except the export window goes to the screen. The draw
        // lists belong to ImGui and stay valid until the next NewFrame().
        ImDrawData screen_draw_data;
        if(export_draw_list != nullptr){
            screen_draw_data.Valid = true;
            screen_draw_data.DisplayPos = frame_draw_data->DisplayPos;
            screen_draw_data.DisplaySize = frame_draw_data->DisplaySize;
            screen_draw_data.FramebufferScale = frame_draw_data->FramebufferScale;
            screen_draw_data.OwnerViewport = frame_draw_data->OwnerViewport;
            for(ImDrawList* list : frame_draw_data->CmdLists){
                if(list == export_draw_list)
                    continue;
                screen_draw_data.CmdLists.push_back(list);
                screen_draw_data.CmdListsCount++;
                screen_draw_data.TotalVtxCount += list->VtxBuffer.Size;
                screen_draw_data.TotalIdxCount += list->IdxBuffer.Size;
            }
        }

        SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, (Uint8)(sv.clear_color.x * 255), (Uint8)(sv.clear_color.y * 255), 
                                (Uint8)(sv.clear_color.z * 255), (Uint8)(sv.clear_color.w * 255));
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(
            export_draw_list != nullptr ? &screen_draw_data : frame_draw_data, renderer);
        SDL_RenderPresent(renderer);

        if(export_capture){
            sv.export_pending = false;
            ensure_export_dir();
            std::string path = unique_export_path(tournament, sv);
            std::string err;
            if(export_drawlist_to_png(renderer, export_draw_list,
                    (int)std::ceil(sv.export_w), (int)std::ceil(sv.export_h),
                    ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), path, err)){
                std::error_code ec;
                std::filesystem::path abs = std::filesystem::absolute(path, ec);
                show_message(sv, "Export Complete", "Saved:\n" + (ec ? path : abs.string()));
            }
            else
                show_message(sv, "Export Failed", err);
        }
    }

    // Cleanup
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
