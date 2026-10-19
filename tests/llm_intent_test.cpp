#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "active_item_cache.h"
#include "activity_actor_definitions.h"
#include "avatar.h"
#include "cata_catch.h"
#include "catacharset.h"
#include "item.h"
#include "item_location.h"
#include "item_pocket.h"
#include "item_uid.h"
#include "json.h"
#include "json_loader.h"
#include "flexbuffer_json.h"
#include "map_selector.h"
#include "visitable.h"
#include "character_id.h"
#include "cata_path.h"
#include "path_info.h"
#include "coordinates.h"
#include "creature.h"
#include "faction.h"
#include "game.h"
#include "llm_intent.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "messages.h"
#include "monster.h"
#include "npc.h"
#include "npctalk.h"
#include "options_helpers.h"
#include "player_helpers.h"
#include "player_activity.h"
#include "point.h"
#include "type_id.h"

static const faction_id faction_your_followers( "your_followers" );
static const furn_str_id furn_f_parkour_bars( "f_parkour_bars" );
static const mtype_id mon_zombie( "mon_zombie" );
static const string_id<npc_template> npc_template_test_talker( "test_talker" );
static const ter_str_id ter_t_rock_wall( "t_rock_wall" );

namespace
{
static npc &spawn_test_npc_at( const point_bub_ms &pos, const std::string &name )
{
    map &here = get_map();
    const character_id model_id = here.place_npc( pos, npc_template_test_talker );
    g->load_npcs();

    npc *guy = g->find_npc( model_id );
    REQUIRE( guy != nullptr );
    clear_character( *guy );
    guy->setpos( here, tripoint_bub_ms( pos, 0 ) );
    guy->name = name;
    return *guy;
}

static void setup_snapshot_test_scene()
{
    clear_avatar();
    clear_npcs();
    clear_map_with_vision();
    clear_vehicles();
    set_time_to_day();
}

static void run_npc_turns( npc &guy, int turns )
{
    for( int turn = 0; turn < turns; ++turn ) {
        guy.set_moves( 100 );
        guy.move();
    }
}
}

TEST_CASE( "llm_intent_snapshot_includes_attitude_and_lettered_targets", "[llm_intent]" )
{
    setup_snapshot_test_scene();
    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.name = "Test Player";
    player_character.setpos( here, tripoint_bub_ms( 48, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );

    npc &neutral_npc = spawn_test_npc_at( point_bub_ms( 52, 50 ), "Neutral NPC" );
    monster &zombie = spawn_test_monster( mon_zombie.str(), tripoint_bub_ms( 54, 50, 0 ) );
    CAPTURE( neutral_npc.disp_name() );
    CAPTURE( zombie.disp_name() );

    const std::string snapshot = llm_intent::build_snapshot_for_test( listener, "Hold there.",
                                 "req-snapshot" );

    CHECK( snapshot.find( "creature legend with attitude and threat level:" ) != std::string::npos );
    CHECK( snapshot.find( "map axes: +x east/right, -x west/left, +y north/up, -y south/down" ) !=
           std::string::npos );
    CHECK( snapshot.find( "-20" ) != std::string::npos );
    CHECK( snapshot.find( "+20" ) != std::string::npos );
    CHECK( snapshot.find( "|.........|.........|.........|.........|" ) != std::string::npos );
    CHECK( snapshot.find( "dy=+20 " ) != std::string::npos );
    CHECK( snapshot.find( "dy=+00 " ) != std::string::npos );
    CHECK( snapshot.find( "dy=-20 " ) != std::string::npos );
    CHECK( snapshot.find( "a ... player friendly threat=" ) != std::string::npos );
    CHECK( snapshot.find( "Neutral NPC" ) != std::string::npos );
    CHECK( snapshot.find( "neutral threat=" ) != std::string::npos );
    CHECK( snapshot.find( "zombie hostile threat=" ) != std::string::npos );
}

TEST_CASE( "llm_intent_prompt_explicitly_allows_lettered_targets", "[llm_intent]" )
{
    const std::string prompt = llm_intent::build_action_prompt_for_test( "Listener NPC",
                               "Do it.", "snapshot" );

    CHECK( prompt.find( "Any creature with a map letter is a valid explicit target handle, including the player, friendlies, neutrals, and hostiles." )
           != std::string::npos );
    CHECK( prompt.find( "attack=a" ) != std::string::npos );
    CHECK( prompt.find( "attack=b" ) != std::string::npos );
}

TEST_CASE( "llm_intent_prompt_uses_delta_move_contract", "[llm_intent]" )
{
    const std::string prompt = llm_intent::build_action_prompt_for_test( "Listener NPC",
                               "Move there.", "snapshot" );

    CHECK( prompt.find( "move=<dx>,<dy> <state>" ) != std::string::npos );
    CHECK( prompt.find( "Positive x is east/right, negative x is west/left, positive y is north/up, and negative y is south/down." )
           != std::string::npos );
    CHECK( prompt.find( "dx column markers and dy row labels" ) != std::string::npos );
    CHECK( prompt.find( "move=0,-5 hold_position" ) != std::string::npos );
    CHECK( prompt.find( "move: S S S S S hold_position" ) == std::string::npos );
}

TEST_CASE( "llm_intent_request_has_no_output_generation_limit", "[llm_intent][response_output]" )
{
    const std::string payload = llm_intent::request_json_for_test();
    const JsonValue loaded = json_loader::from_string( payload );
    const JsonObject request = loaded.get_object();
    CHECK( request.get_string( "request_id" ) == "request-producer-control" );
    // Consume the unchanged local sampling fields with the strict native reader.
    ( void )request.get_float( "temperature" );
    ( void )request.get_float( "top_p" );
    ( void )request.get_float( "repetition_penalty" );
    CHECK( request.get_string( "prompt" ) == "unchanged prompt" );
    CHECK( request.get_string( "snapshot" ) == "unchanged snapshot" );
    CHECK_FALSE( request.has_member( "max_tokens" ) );
    CHECK_FALSE( request.has_member( "max_new_tokens" ) );
    CHECK_FALSE( request.has_member( "max_length" ) );
}

TEST_CASE( "llm_intent_can_parse_delta_move_fields", "[llm_intent]" )
{
    point delta;
    std::string terminal_state;
    std::string error;

    CHECK( llm_intent::parse_move_field_for_test( "move=4,-2 hold_position", delta,
            terminal_state, error ) );
    CHECK( delta == point( 4, -2 ) );
    CHECK( terminal_state == "hold_position" );

    CHECK( llm_intent::parse_move_field_for_test( "move=-1,3 wait_here", delta,
            terminal_state, error ) );
    CHECK( delta == point( -1, 3 ) );
    CHECK( terminal_state == "wait_here" );

    CHECK( llm_intent::parse_move_field_for_test( "MOVE= -20,+20 WAIT_HERE", delta,
            terminal_state, error ) );
    CHECK( delta == point( -20, 20 ) );
    CHECK( terminal_state == "wait_here" );

    CHECK_FALSE( llm_intent::parse_move_field_for_test( "move: E E N hold_position", delta,
                 terminal_state, error ) );
    CHECK( error == "Move field must use move=<dx>,<dy> <state>." );

    CHECK_FALSE( llm_intent::parse_move_field_for_test( "move E E N hold_position", delta,
                 terminal_state, error ) );
    CHECK( error == "Move field must use move=<dx>,<dy> <state>." );

    CHECK_FALSE( llm_intent::parse_move_field_for_test( "move=4 east hold_position", delta,
                 terminal_state, error ) );
    CHECK_FALSE( error.empty() );

    CHECK_FALSE( llm_intent::parse_move_field_for_test( "move=21,0 hold_position", delta,
                 terminal_state, error ) );
    CHECK( error == "Move field delta must stay within the snapshot map (-20..20)." );

    CHECK( llm_intent::parse_move_field_for_test( "move=1,0 wait_here", delta,
            terminal_state, error ) );
    CHECK( error.empty() );

    CHECK_FALSE( llm_intent::parse_move_field_for_test( "move=-2147483648,0 wait_here", delta,
                 terminal_state, error ) );
    CHECK( error == "Move field delta must stay within the snapshot map (-20..20)." );
}

TEST_CASE( "llm_intent_action_csv_applies_move_and_attack_contract", "[llm_intent]" )
{
    std::vector<std::string> actions;
    std::string attack_target;
    std::optional<point> move_delta;
    std::string terminal_state;
    std::string error;

    CHECK( llm_intent::parse_action_csv_for_test(
               "On it|move=4,-2 wait_here|equip_gun", actions, attack_target,
               move_delta, terminal_state, error ) );
    CHECK( actions == std::vector<std::string> { "move=4,-2 wait_here", "equip_gun" } );
    CHECK( attack_target.empty() );
    CHECK( move_delta == point( 4, -2 ) );
    CHECK( terminal_state == "wait_here" );

    CHECK( llm_intent::parse_action_csv_for_test(
               " | Inspecting inventory... I'm carrying a sharpened rebar. | equip_melee",
               actions, attack_target, move_delta, terminal_state, error ) );
    CHECK( actions == std::vector<std::string> { "equip_melee" } );
    CHECK( attack_target.empty() );
    CHECK_FALSE( move_delta.has_value() );
    CHECK( llm_intent::parse_action_csv_speech_for_test(
               " | Inspecting inventory... I'm carrying a sharpened rebar. | equip_melee" ) ==
           "Inspecting inventory... I'm carrying a sharpened rebar." );

    CHECK_FALSE( llm_intent::parse_action_csv_for_test(
                     "||Speech|equip_melee", actions, attack_target,
                     move_delta, terminal_state, error ) );
    CHECK( error == "CSV speech field missing." );

    CHECK_FALSE( llm_intent::parse_action_csv_for_test(
                     "|Speech|equip_melee|follow_close|panic_off|wait_here", actions, attack_target,
                     move_delta, terminal_state, error ) );
    CHECK( error == "CSV has too many action fields." );

    CHECK_FALSE( llm_intent::parse_action_csv_for_test(
                     "|Speech|equip_melee|", actions, attack_target,
                     move_delta, terminal_state, error ) );
    CHECK( error == "CSV action token is invalid." );

    CHECK( llm_intent::parse_action_csv_for_test(
               "Engaging|attack=a|equip_gun|follow_close panic_off", actions, attack_target,
               move_delta, terminal_state, error ) );
    CHECK( actions == std::vector<std::string> { "equip_gun", "follow_close", "panic_off" } );
    CHECK( attack_target == "a" );
    CHECK_FALSE( move_delta.has_value() );

    CHECK_FALSE( llm_intent::parse_action_csv_for_test(
                     "No|move=1,0 wait_here|move=2,0 hold_position", actions, attack_target,
                     move_delta, terminal_state, error ) );
    CHECK( error == "CSV move field repeated." );

    CHECK_FALSE( llm_intent::parse_action_csv_for_test(
                     "No|move=21,0 wait_here", actions, attack_target,
                     move_delta, terminal_state, error ) );
    CHECK( error == "Move field delta must stay within the snapshot map (-20..20)." );

    CHECK( llm_intent::parse_action_csv_for_test(
               "Moving|move=1,0 wait_here", actions, attack_target,
               move_delta, terminal_state, error ) );
    CHECK( error.empty() );
    CHECK( move_delta == point( 1, 0 ) );

    CHECK_FALSE( llm_intent::parse_action_csv_for_test(
                     "No|bogus follow_close", actions, attack_target,
                     move_delta, terminal_state, error ) );
    CHECK( error == "CSV action token is invalid." );

    const std::string normalized = llm_intent::normalize_csv_separators_for_test(
                                       "Moving+move=-20,+20 wait_here" );
    CHECK( normalized == "Moving|move=-20,+20 wait_here" );
    CHECK( llm_intent::parse_action_csv_for_test(
               normalized, actions, attack_target, move_delta, terminal_state, error ) );
    CHECK( move_delta == point( -20, 20 ) );
}

TEST_CASE( "llm_intent_leading_separator_reaches_visible_speech_and_action_route",
           "[llm_intent]" )
{
    override_option opt_llm_intent( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();

    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.setpos( here, tripoint_bub_ms( 48, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    REQUIRE( listener.is_player_ally() );
    Messages::clear_messages();

    llm_intent::process_response_for_test(
        listener, "req-leading-separator", "What are you carrying?",
        " | Inspecting inventory... I'm carrying a sharpened rebar. | equip_melee" );

    const std::vector<std::pair<std::string, std::string>> messages =
                Messages::recent_messages( 0 );
    REQUIRE_FALSE( messages.empty() );
    CHECK( messages.back().second ==
           "Listener NPC says: \"Inspecting inventory... I'm carrying a sharpened rebar.\"" );
    CHECK( listener.has_llm_intent_actions() );
    CHECK( listener.get_llm_intent_actions_for_test() ==
           std::vector<llm_intent_action> { llm_intent_action::equip_melee } );
    CHECK_FALSE( llm_intent::has_request_state_for_test( listener, "req-leading-separator" ) );

    const std::vector<npc::llm_intent_memory_entry> memory = listener.get_llm_intent_memory();
    REQUIRE_FALSE( memory.empty() );
    CHECK( memory.back().player_utterance == "What are you carrying?" );
    CHECK( memory.back().npc_response ==
           "Inspecting inventory... I'm carrying a sharpened rebar." );
    CHECK( memory.back().actions == std::vector<std::string> { "equip_melee" } );
}

TEST_CASE( "llm_intent_final_suffix_reaches_native_speech_and_actions",
           "[llm_intent][response_output]" )
{
    const std::vector<std::pair<std::string, bool>> samples = {
        { "Ready.|wait_here", true },
        { "<think>PRIVATE_THOUGHT|attack=a|panic_on</think>Ready.|wait_here", true },
        { "<|channel>thought\nPRIVATE_THOUGHT|panic_on<channel|>Ready.|wait_here", true },
        { "<think>PRIVATE_THOUGHT|attack=a", false },
        { "<|channel>thought\nPRIVATE_THOUGHT|panic_on", false },
        { "<think>PRIVATE_THOUGHT|attack=a</think>", false }
    };
    for( size_t i = 0; i < samples.size(); ++i ) {
        DYNAMIC_SECTION( "response case " << i ) {
            override_option enabled( "LLM_INTENT_ENABLE", "true" );
            override_option debug_ui( "DEBUG_LLM_INTENT_UI", "true" );
            setup_snapshot_test_scene();
            npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
            listener.set_fac( faction_your_followers );
            Messages::clear_messages();
            llm_intent::process_response_for_test( listener, "final-suffix", "Wait here.",
                                                 samples[i].first );
            if( samples[i].second ) {
                CHECK( listener.get_llm_intent_actions_for_test() ==
                       std::vector<llm_intent_action> { llm_intent_action::wait_here } );
                REQUIRE_FALSE( listener.get_llm_intent_memory().empty() );
                CHECK( listener.get_llm_intent_memory().back().npc_response == "Ready." );
            } else {
                CHECK_FALSE( listener.has_llm_intent_actions() );
            }
            for( const auto &message : Messages::recent_messages( 0 ) ) {
                CHECK( message.second.find( "PRIVATE_THOUGHT" ) == std::string::npos );
            }
        }
    }
}

TEST_CASE( "llm_intent_secondary_reader_never_selects_reasoning_items",
           "[llm_intent][response_output]" )
{
    const std::vector<std::string> allowed = { "safe item", "reasoning item" };
    CHECK( llm_intent::parse_look_around_response_for_test(
               "<think>item_2</think>item_1", allowed ) == std::vector<std::string> { "safe item" } );
    CHECK( llm_intent::parse_look_around_response_for_test(
               "<|channel>thought\nitem_2<channel|>item_1", allowed ) ==
           std::vector<std::string> { "safe item" } );
    CHECK( llm_intent::parse_look_around_response_for_test( "<think>item_2", allowed ).empty() );
}

TEST_CASE( "llm_intent_machine_event_log_preserves_utf8_json_punctuation", "[llm_intent]" )
{
    const std::string curly_quotes = "\xE2\x80\x9Cquoted\xE2\x80\x9D";
    const std::string payload = "[CAOL_EVENT] action_status npc=\"Ada " + curly_quotes +
                                "\" kind=\"look_inventory\"";

    const std::string prepared = llm_intent::prepare_event_log_payload_for_test( payload );

    CHECK( prepared == payload + "\n\n" );
    CHECK( prepared.find( curly_quotes ) != std::string::npos );
    CHECK( prepared.find( "npc=\"Ada \"quoted\"\"" ) == std::string::npos );
}

TEST_CASE( "llm_intent_look_around_filters_and_caps_selected_items", "[llm_intent]" )
{
    CHECK( llm_intent::look_around_selection_limit_for_test() == 4 );

    const std::vector<std::string> selected = llm_intent::parse_look_around_response_for_test(
    "item_1:2, unknown, item_2, item_3, item_4, item_5, item_1", {
        "adhesive bandage", "9x19mm JHP, reloaded",
        "Glock 9x19mm 15-round magazine", "small plastic bag", "combat knife"
    } );
    REQUIRE( selected.size() == 4 );
    CHECK( selected[0] == "adhesive bandage" );
    CHECK( selected[1] == "9x19mm JHP, reloaded" );
    CHECK( selected[2] == "Glock 9x19mm 15-round magazine" );
    CHECK( selected[3] == "small plastic bag" );
}

TEST_CASE( "llm_intent_resolves_move_deltas_to_snapshot_targets", "[llm_intent]" )
{
    const tripoint_abs_ms origin( 100, 200, 7 );
    CHECK( llm_intent::resolve_move_target_for_test( origin, point( 4, -2 ) ) ==
           tripoint_abs_ms( 104, 202, 7 ) );
    CHECK( llm_intent::resolve_move_target_for_test( origin, point( -1, 3 ) ) ==
           tripoint_abs_ms( 99, 197, 7 ) );
}

TEST_CASE( "llm_intent_move_targets_reuse_existing_tile_pathing", "[llm_intent]" )
{
    override_option opt_llm_intent( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();

    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.setpos( here, tripoint_bub_ms( 46, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );

    const tripoint_abs_ms target = llm_intent::resolve_move_target_for_test( listener.pos_abs(),
                                   point( 2, 1 ) );
    listener.set_llm_intent_move_target( target, llm_intent_action::wait_here );
    run_npc_turns( listener, 6 );

    CHECK( listener.pos_abs() == target );
    CHECK_FALSE( listener.goto_to_this_pos.has_value() );
    CHECK( listener.mission == NPC_MISSION_GUARD_ALLY );
    CHECK( listener.get_attitude() == NPCATT_NULL );

    clear_npcs();
}

TEST_CASE( "llm_intent_unreachable_move_target_clears_stale_path", "[llm_intent]" )
{
    override_option opt_llm_intent( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();

    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.setpos( here, tripoint_bub_ms( 44, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    listener.set_attitude( NPCATT_FOLLOW );

    const tripoint_bub_ms stale_path_target( 54, 50, 0 );
    REQUIRE( listener.update_path( stale_path_target ) );
    REQUIRE_FALSE( listener.path.empty() );

    const tripoint_bub_ms unreachable_target( 50, 49, 0 );
    here.ter_set( unreachable_target, ter_t_rock_wall );
    REQUIRE_FALSE( listener.update_path( unreachable_target, false, false ) );
    REQUIRE_FALSE( listener.path.empty() );

    const tripoint_abs_ms ordered_target = here.get_abs( unreachable_target );
    const tripoint_abs_ms initial_position = listener.pos_abs();
    listener.set_llm_intent_move_target( ordered_target, llm_intent_action::wait_here );
    run_npc_turns( listener, 1 );

    CHECK( listener.pos_abs() == initial_position );
    CHECK_FALSE( listener.goto_to_this_pos.has_value() );
    CHECK( listener.path.empty() );

    const int initial_player_distance = rl_dist( listener.pos_bub(), player_character.pos_bub() );
    run_npc_turns( listener, 1 );
    CHECK( rl_dist( listener.pos_bub(), player_character.pos_bub() ) < initial_player_distance );

    clear_npcs();
}

TEST_CASE( "llm_intent_hold_position_releases_when_player_gets_far", "[llm_intent]" )
{
    override_option opt_llm_intent( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();

    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.setpos( here, tripoint_bub_ms( 48, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );

    listener.set_llm_intent_move_target( listener.pos_abs(), llm_intent_action::hold_position );
    run_npc_turns( listener, 1 );

    REQUIRE( listener.mission == NPC_MISSION_GUARD_ALLY );
    REQUIRE( listener.get_attitude() == NPCATT_NULL );

    player_character.setpos( here, tripoint_bub_ms( 80, 80, 0 ) );
    run_npc_turns( listener, 1 );

    CHECK( listener.get_attitude() == NPCATT_FOLLOW );
    CHECK( listener.mission == NPC_MISSION_NULL );
    CHECK_FALSE( listener.guard_pos.has_value() );

    clear_npcs();
}

TEST_CASE( "llm_intent_wait_here_stays_guarded_when_player_gets_far", "[llm_intent]" )
{
    override_option opt_llm_intent( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();

    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.setpos( here, tripoint_bub_ms( 48, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );

    listener.set_llm_intent_move_target( listener.pos_abs(), llm_intent_action::wait_here );
    run_npc_turns( listener, 1 );

    REQUIRE( listener.mission == NPC_MISSION_GUARD_ALLY );
    REQUIRE( listener.get_attitude() == NPCATT_NULL );

    player_character.setpos( here, tripoint_bub_ms( 80, 80, 0 ) );
    run_npc_turns( listener, 1 );

    CHECK( listener.get_attitude() == NPCATT_NULL );
    CHECK( listener.mission == NPC_MISSION_GUARD_ALLY );
    CHECK( listener.guard_pos == listener.pos_abs() );

    clear_npcs();
}

TEST_CASE( "llm_intent_snapshot_request_resolves_lettered_neutral_targets", "[llm_intent]" )
{
    override_option opt_llm_intent( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();

    map &here = get_map();
    avatar &player_character = get_avatar();
    player_character.setpos( here, tripoint_bub_ms( 46, 50, 0 ) );

    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );

    npc &neutral_npc = spawn_test_npc_at( point_bub_ms( 54, 50 ), "Neutral NPC" );
    REQUIRE( listener.attitude_to( neutral_npc ) == Creature::Attitude::NEUTRAL );
    REQUIRE( here.furn_set( neutral_npc.pos_bub(), furn_f_parkour_bars ) );
    REQUIRE( listener.sees( here, neutral_npc ) );
    REQUIRE( here.move_cost( neutral_npc.pos_bub() ) * 50 > 100 );

    const std::string snapshot = llm_intent::build_snapshot_for_test(
                                     listener, "Attack the neutral target.", "req-target" );
    CAPTURE( snapshot );
    const std::string neutral_legend = " ... " + neutral_npc.disp_name() + " neutral threat=";
    const size_t neutral_legend_pos = snapshot.find( neutral_legend );
    REQUIRE( neutral_legend_pos != std::string::npos );
    REQUIRE( neutral_legend_pos >= 1 );
    REQUIRE( ( neutral_legend_pos == 1 || snapshot[neutral_legend_pos - 2] == '\n' ) );
    const char obstructed_handle = snapshot[neutral_legend_pos - 1];
    REQUIRE( obstructed_handle >= 'A' );
    REQUIRE( obstructed_handle <= 'Z' );
    const char normalized_handle = static_cast<char>( obstructed_handle - 'A' + 'a' );

    std::vector<std::string> actions;
    std::string attack_target;
    std::optional<point> move_delta;
    std::string terminal_state;
    std::string error;
    REQUIRE( llm_intent::parse_action_csv_for_test(
                 std::string( "Engaging|attack=" ) + obstructed_handle, actions, attack_target,
                 move_delta, terminal_state, error ) );
    REQUIRE( attack_target == std::string( 1, normalized_handle ) );

    listener.set_llm_intent_actions( {}, "stale-request", attack_target );
    listener.move();
    CHECK( listener.attitude_to( neutral_npc ) == Creature::Attitude::NEUTRAL );

    listener.set_llm_intent_actions( {}, "req-target", attack_target );
    listener.move();

    CHECK( listener.current_target() == &neutral_npc );
    CHECK( listener.attitude_to( neutral_npc ) == Creature::Attitude::HOSTILE );
}

TEST_CASE( "llm_intent_snapshot_does_not_reuse_target_handles_after_z", "[llm_intent]" )
{
    setup_snapshot_test_scene();

    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 5, 5, 0 ) );
    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );

    int spawned = 0;
    for( int y = 45; y <= 55 && spawned < 27; ++y ) {
        for( int x = 45; x <= 55 && spawned < 27; ++x ) {
            const tripoint_bub_ms pos( x, y, 0 );
            if( pos == listener.pos_bub() ) {
                continue;
            }
            spawn_test_monster( mon_zombie.str(), pos );
            ++spawned;
        }
    }
    REQUIRE( spawned == 27 );

    const std::string snapshot = llm_intent::build_snapshot_for_test(
                                     listener, "Watch the horde.", "req-dense" );
    const size_t map_start = snapshot.find( "map:\n" );
    REQUIRE( map_start != std::string::npos );

    std::istringstream map_stream( snapshot.substr( map_start + 5 ) );
    std::string row;
    REQUIRE( std::getline( map_stream, row ) );
    REQUIRE( std::getline( map_stream, row ) );

    std::array<int, 26> handle_counts{};
    int unlettered_creatures = 0;
    int map_rows = 0;
    while( std::getline( map_stream, row ) && row.rfind( "dy=", 0 ) == 0 ) {
        const size_t grid_start = row.find( ' ' );
        REQUIRE( grid_start != std::string::npos );
        const std::string grid = row.substr( grid_start + 1 );
        REQUIRE( grid.size() == 41 );
        ++map_rows;
        for( char glyph : grid ) {
            if( glyph >= 'a' && glyph <= 'z' ) {
                ++handle_counts[static_cast<size_t>( glyph - 'a' )];
            } else if( glyph >= 'A' && glyph <= 'Z' ) {
                ++handle_counts[static_cast<size_t>( glyph - 'A' )];
            } else if( glyph == '?' ) {
                ++unlettered_creatures;
            }
        }
    }

    CHECK( map_rows == 41 );
    CHECK( unlettered_creatures == 1 );
    for( int count : handle_counts ) {
        CHECK( count == 1 );
    }

    const size_t creature_legend_start = snapshot.find(
            "creature legend with attitude and threat level:\n" );
    const size_t map_axes_start = snapshot.find( "map axes:", creature_legend_start );
    REQUIRE( creature_legend_start != std::string::npos );
    REQUIRE( map_axes_start != std::string::npos );
    const std::string creature_legend = snapshot.substr(
                                            creature_legend_start, map_axes_start - creature_legend_start );
    CHECK( creature_legend.find( "? ... zombie" ) == std::string::npos );

    clear_creatures();
}

TEST_CASE( "llm_debug_logging_is_opt_in", "[llm_intent][logging]" )
{
    const std::filesystem::path previous = PATH_INFO::base_path().get_unrelative_path();
    const std::filesystem::path temporary = std::filesystem::temp_directory_path() /
                                            ( "caol-llm-log-test-" + std::to_string(
                                                    std::chrono::steady_clock::now().time_since_epoch().count() ) );
    struct restore_paths {
        std::filesystem::path previous;
        std::filesystem::path temporary;
        ~restore_paths() {
            PATH_INFO::init_base_path( previous.string() );
            std::error_code ignored;
            std::filesystem::remove_all( temporary, ignored );
        }
    } cleanup{ previous, temporary };
    PATH_INFO::init_base_path( temporary.string() );
    const std::filesystem::path events = temporary / "config" / "llm_intent_events.log";
    {
        override_option disabled( "DEBUG_LLM_INTENT_LOG", "false" );
        llm_intent::log_event( "disabled diagnostic" );
        CHECK_FALSE( std::filesystem::exists( temporary / "config" ) );
    }
    {
        override_option enabled( "DEBUG_LLM_INTENT_LOG", "true" );
        llm_intent::log_event( "enabled diagnostic" );
        REQUIRE( std::filesystem::exists( events ) );
        std::ifstream input( events );
        const std::string contents( ( std::istreambuf_iterator<char>( input ) ),
                                    std::istreambuf_iterator<char>() );
        CHECK( contents.find( "enabled diagnostic" ) != std::string::npos );
        CHECK( contents.find( "disabled diagnostic" ) == std::string::npos );
    }
    const auto size = std::filesystem::file_size( events );
    {
        override_option disabled( "DEBUG_LLM_INTENT_LOG", "false" );
        llm_intent::log_event( "disabled again" );
        CHECK( std::filesystem::file_size( events ) == size );
    }
}

TEST_CASE( "recorded_panic_reply_preserves_action_through_lenient_parse", "[llm_intent]" )
{
    // Retained E4B reply, runner log line 646. This isolates parser behavior;
    // it does not turn the old native run into a successful flee test.
    const std::string reply = "Panic! I'm boltin' for the hills!|panic_on|move=0,-50 panic_on";
    std::vector<std::string> actions;
    std::string target;
    std::optional<point> delta;
    std::string terminal;
    std::string error;
    CHECK_FALSE( llm_intent::parse_action_csv_for_test( reply, actions, target, delta,
                 terminal, error ) );
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();
    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    llm_intent::process_response_for_test( listener, "recorded-panic", "panic and flee", reply );
    CHECK( listener.get_llm_intent_actions_for_test() ==
           std::vector<llm_intent_action> { llm_intent_action::panic_on } );
}

TEST_CASE( "llm_response_error_diagnostics_are_categorized_and_sanitized", "[llm_intent][logging]" )
{
    const std::string error =
        "Runner process exited (signal=13).\nRunner log tail: private prompt=do not emit this";
    CHECK( llm_intent::classify_response_error_for_test( error ) == "runner_process_exit" );
    const std::string sanitized = llm_intent::sanitize_response_error_for_test( error );
    CHECK( sanitized == "Runner process exited (signal=13)." );
    CHECK( sanitized.find( "private prompt" ) == std::string::npos );
    CHECK( llm_intent::classify_response_error_for_test( "Failed to write to runner stdin (Broken pipe)." )
           ==
           "runner_stdin_write" );
    CHECK( llm_intent::classify_response_error_for_test( "Runner stdout closed (still_running)." ) ==
           "runner_stdout_eof" );
    CHECK( llm_intent::should_retry_response_for_test( "Runner stdout closed (still_running)." ) );
    CHECK_FALSE( llm_intent::should_retry_response_for_test( "Runner response timed out." ) );
    CHECK_FALSE( llm_intent::should_retry_response_for_test( "Runner process exited (signal=13)." ) );
}


TEST_CASE( "native_wear_transfers_the_selected_tree_and_preserves_copy_callers",
           "[llm_intent][companion_repair][wear_transfer]" )
{
    setup_snapshot_test_scene();
    map &here = get_map();
    avatar &who = get_avatar();
    who.setpos( here, tripoint_bub_ms( 48, 50, 0 ) );
    item coat( itype_id( "jacket_leather" ), calendar::turn );
    REQUIRE( coat.put_in( item( itype_id( "rock" ), calendar::turn ),
                          pocket_type::CONTAINER ).success() );
    const std::string storage = GENERATE( "ground", "wielded", "container", "copy", "unsorted_copy" );
    CAPTURE( storage );
    if( storage == "copy" || storage == "unsorted_copy" ) {
        const int64_t coat_uid = coat.uid().get_value();
        const int64_t rock_uid = coat.only_item().uid().get_value();
        const auto result = who.wear_item( coat, false, true, storage == "copy" );
        REQUIRE( result.has_value() );
        CHECK( ( *result )->uid().get_value() != coat_uid );
        CHECK( ( *result )->only_item().uid().get_value() != rock_uid );
        CHECK( coat.uid().get_value() == coat_uid );
        CHECK( coat.only_item().uid().get_value() == rock_uid );
        return;
    }
    item_location selected;
    item_location container;
    if( storage == "ground" ) {
        item &placed = here.add_item_or_charges( who.pos_bub(), coat );
        selected = item_location( map_cursor( who.pos_bub() ), &placed );
    } else if( storage == "wielded" ) {
        REQUIRE( who.wield( coat ) );
        selected = who.get_wielded_item();
    } else {
        REQUIRE( who.wear_item( item( itype_id( "backpack" ), calendar::turn ), false ) );
        container = who.worn.top_items_loc( who ).front();
        REQUIRE( container->put_in( coat, pocket_type::CONTAINER ).success() );
        selected = item_location( container, &container->only_item() );
    }
    const int64_t coat_uid = selected->uid().get_value();
    const int64_t rock_uid = selected->only_item().uid().get_value();
    const auto result = who.wear( selected, false );
    REQUIRE( result.has_value() );
    CHECK( ( *result )->uid().get_value() == coat_uid );
    CHECK( ( *result )->only_item().uid().get_value() == rock_uid );
    CHECK( who.is_worn( **result ) );
    CHECK( who.amount_worn( itype_id( "jacket_leather" ) ) == 1 );
    std::ostringstream saved;
    JsonOut out( saved );
    ( **result ).serialize( out );
    JsonValue loaded = json_loader::from_string( saved.str() );
    item restored;
    restored.deserialize( loaded );
    CHECK( restored.uid().get_value() == coat_uid );
    CHECK( restored.only_item().uid().get_value() == rock_uid );
    CHECK_FALSE( who.get_wielded_item() );
    CHECK( here.i_at( who.pos_bub() ).empty() );
    if( container ) {
        CHECK( container->all_items_top().empty() );
    }
    CHECK_FALSE( who.wear( item_location( who, & **result ), false ).has_value() );
    CHECK( ( *result )->uid().get_value() == coat_uid );
}

TEST_CASE( "native_wear_refusal_keeps_owned_item_identity_and_contents",
           "[llm_intent][companion_repair][wear_transfer]" )
{
    setup_snapshot_test_scene();
    avatar &who = get_avatar();
    item box( itype_id( "box_small" ), calendar::turn );
    REQUIRE( box.put_in( item( itype_id( "rock" ), calendar::turn ),
                         pocket_type::CONTAINER ).success() );
    SECTION( "unconsumed rvalue" ) {
        const int64_t box_uid = box.uid().get_value();
        const int64_t rock_uid = box.only_item().uid().get_value();
        CHECK_FALSE( who.wear_item( std::move( box ), false ) );
        CHECK( box.uid().get_value() == box_uid );
        CHECK( box.only_item().uid().get_value() == rock_uid );
    }
    SECTION( "ground or container rollback conserves the entire tree" ) {
        REQUIRE( who.wear_item( item( itype_id( "backpack" ), calendar::turn ), false ) );
        item_location backpack = who.worn.top_items_loc( who ).front();
        const int64_t backpack_uid = backpack->uid().get_value();
        const bool ground = GENERATE( false, true );
        item_location selected;
        if( ground ) {
            item &placed = get_map().add_item_or_charges( who.pos_bub(), box );
            selected = item_location( map_cursor( who.pos_bub() ), &placed );
        } else {
            REQUIRE( backpack->put_in( box, pocket_type::CONTAINER ).success() );
            selected = item_location( backpack, &backpack->only_item() );
        }
        const int64_t box_uid = selected->uid().get_value();
        const int64_t rock_uid = selected->only_item().uid().get_value();
        CHECK_FALSE( who.wear( selected, false ) );
        int boxes = 0;
        who.visit_items( [&]( item * it, item * ) {
            if( it->uid().get_value() == box_uid ) {
                ++boxes;
                CHECK( it->only_item().uid().get_value() == rock_uid );
            }
            return VisitResponse::NEXT;
        } );
        CHECK( boxes == ( ground ? 0 : 1 ) );
        if( ground ) {
            REQUIRE( get_map().i_at( who.pos_bub() ).size() == 1 );
            const item &retained = *get_map().i_at( who.pos_bub() ).begin();
            CHECK( retained.uid().get_value() == box_uid );
            CHECK( retained.only_item().uid().get_value() == rock_uid );
        } else {
            CHECK( get_map().i_at( who.pos_bub() ).empty() );
            CHECK( backpack->only_item().uid().get_value() == box_uid );
        }
        CHECK( backpack->uid().get_value() == backpack_uid );
    }
    SECTION( "wielded rollback" ) {
        REQUIRE( who.wield( box ) );
        const int64_t box_uid = who.get_wielded_item()->uid().get_value();
        const int64_t rock_uid = who.get_wielded_item()->only_item().uid().get_value();
        CHECK_FALSE( who.wear( who.get_wielded_item(), false ) );
        REQUIRE( who.get_wielded_item() );
        CHECK( who.get_wielded_item()->uid().get_value() == box_uid );
        CHECK( who.get_wielded_item()->only_item().uid().get_value() == rock_uid );
    }
}

TEST_CASE( "llm_inventory_wear_consumes_original_once_and_resolves_later_sections",
           "[llm_intent][companion_repair]" )
{
    setup_snapshot_test_scene();
    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    item coat( itype_id( "jacket_leather" ), calendar::turn );
    REQUIRE( coat.put_in( item( itype_id( "rock" ), calendar::turn ),
                          pocket_type::CONTAINER ).success() );
    REQUIRE( listener.wield( coat ) );
    const int64_t coat_uid = listener.get_wielded_item()->uid().get_value();
    const int64_t rock_uid = listener.get_wielded_item()->only_item().uid().get_value();
    const std::string prompt = llm_intent::build_inventory_prompt_for_test( listener,
        "Wear the jacket" );
    REQUIRE( prompt.find( "id=\"item_1\"" ) != std::string::npos );
    llm_intent::process_inventory_response_for_test( listener, "wear: item_1" );
    CHECK_FALSE( listener.get_wielded_item() );
    CHECK( listener.amount_worn( itype_id( "jacket_leather" ) ) == 1 );
    int coats = 0;
    listener.visit_items( [&]( item * it, item * ) {
        if( it->typeId() == itype_id( "jacket_leather" ) ) {
            ++coats;
            CHECK( it->uid().get_value() == coat_uid );
            CHECK( it->only_item().uid().get_value() == rock_uid );
        }
        return VisitResponse::NEXT;
    } );
    CHECK( coats == 1 );
    SECTION( "repeat refusal" ) {
        llm_intent::process_inventory_response_for_test( listener, "wear: item_1" );
        CHECK( listener.amount_worn( itype_id( "jacket_leather" ) ) == 1 );
    }
    SECTION( "same-response transfer then drop uses new owner" ) {
        // A second garment changes storage before a later section resolves it.
        item shirt( itype_id( "tshirt" ), calendar::turn );
        shirt.set_var( "item_label", "transfer shirt" );
        REQUIRE( listener.wield( shirt ) );
        const int64_t shirt_uid = listener.get_wielded_item()->uid().get_value();
        const std::string offered = llm_intent::build_inventory_prompt_for_test( listener,
            "wear then drop" );
        const size_t label_pos = offered.find( "transfer shirt" );
        REQUIRE( label_pos != std::string::npos );
        const std::string id_prefix = "<Item id=\"";
        const size_t item_pos = offered.rfind( id_prefix, label_pos );
        REQUIRE( item_pos != std::string::npos );
        const size_t id_start = item_pos + id_prefix.size();
        const std::string selected = offered.substr( id_start, offered.find( '\"', id_start ) - id_start );
        override_option opt_ui( "DEBUG_LLM_INTENT_UI", "true" );
        Messages::clear_messages();
        llm_intent::process_inventory_response_for_test( listener,
                "wear: " + selected + " | drop: " + selected );
        // npc::drop processes its activity immediately.  Check the actual
        // physical result rather than assuming an avatar-style pending activity.
        CHECK( listener.amount_worn( itype_id( "tshirt" ) ) == 0 );
        int shirts_dropped = 0;
        for( const item &it : get_map().i_at( listener.pos_bub() ) ) {
            shirts_dropped += it.typeId() == itype_id( "tshirt" ) &&
                              it.get_var( "item_label" ) == "transfer shirt";
        }
        CHECK( shirts_dropped == 1 );
        int completed = 0;
        for( const auto &message : Messages::recent_messages( 0 ) ) {
            CHECK( message.second.find( "inventory.item_missing" ) == std::string::npos );
            completed += message.second.find( "LLM look_inventory completed" ) != std::string::npos;
        }
        CHECK( completed == 2 );
        CHECK( shirt_uid > 0 );
    }
}

TEST_CASE( "llm_item_labels_stay_utf8_through_inventory_world_and_action_messages",
           "[llm_intent][companion_repair][utf8_label]" )
{
    override_option opt_ui( "DEBUG_LLM_INTENT_UI", "true" );
    setup_snapshot_test_scene();
    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    item can( itype_id( "can_drink" ), calendar::turn );
    REQUIRE_FALSE( can.get_container_pockets().empty() );
    can.get_container_pockets().front()->settings.whitelist_item( itype_id( "water" ) );
    const std::string label = "café 界 can";
    can.set_var( "item_label", label );
    REQUIRE( can.tname().find( "⁺" ) != std::string::npos );
    REQUIRE( listener.wield( can ) );
    const std::string inventory = llm_intent::build_inventory_prompt_for_test( listener, "Inspect" );
    CHECK( inventory.find( label + "⁺" ) != std::string::npos );
    CHECK( utf32_to_utf8( utf8_to_utf32( inventory ) ) == inventory );
    item &placed = get_map().add_item_or_charges( listener.pos_bub(), can );
    const std::string world = llm_intent::build_look_around_prompt_for_test( listener, "Look" );
    CHECK( world.find( label + "⁺" ) != std::string::npos );
    CHECK( utf32_to_utf8( utf8_to_utf32( world ) ) == world );
    Messages::clear_messages();
    llm_intent::process_inventory_response_for_test( listener, "wear: item_1" );
    const auto messages = Messages::recent_messages( 0 );
    bool saw_label = false;
    for( const auto &message : messages ) {
        CHECK( utf32_to_utf8( utf8_to_utf32( message.second ) ) == message.second );
        saw_label |= message.second.find( label + "⁺" ) != std::string::npos;
    }
    CHECK( saw_label );
    std::ostringstream saved;
    JsonOut out( saved );
    placed.serialize( out );
    CHECK( utf32_to_utf8( utf8_to_utf32( saved.str() ) ) == saved.str() );
    item restored;
    JsonValue loaded = json_loader::from_string( saved.str() );
    restored.deserialize( loaded );
    CHECK( restored.tname() == placed.tname() );
    CHECK( restored.uid().get_value() == placed.uid().get_value() );
}


TEST_CASE( "look_around_item_labels_match_through_native_pickup",
           "[llm_intent][companion_repair][utf8_pickup]" )
{
    override_option opt_ui( "DEBUG_LLM_INTENT_UI", "true" );
    override_option opt_llm( "LLM_INTENT_ENABLE", "true" );
    setup_snapshot_test_scene();
    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    const int variant = GENERATE( 0, 1, 2 );
    CAPTURE( variant );
    item box( itype_id( "box_small" ), calendar::turn );
    const std::string label = variant == 2 ? "café 界 box" : "supply box";
    box.set_var( "item_label", label );
    if( variant != 0 ) {
        box.get_container_pockets().front()->settings.whitelist_item( itype_id( "protein_bar_evac" ) );
    }
    REQUIRE( box.put_in( item( itype_id( "protein_bar_evac" ), calendar::turn ),
                         pocket_type::CONTAINER ).success() );
    item &placed = get_map().add_item_or_charges( listener.pos_bub(), box );
    const std::string expected = label + ( variant == 0 ? "" : "⁺" );
    // Isolate label matching from distance, visibility and intake eligibility.
    REQUIRE( get_map().sees_some_items( listener.pos_bub(), listener ) );
    REQUIRE( listener.sees( get_map(), listener.pos_bub() ) );
    REQUIRE( listener.can_wield( placed ).success() );
    REQUIRE( listener.would_take_that( placed, listener.pos_bub() ) );
    const std::string prompt = llm_intent::build_look_around_prompt_for_test( listener,
                               "Pick up one supply box" );
    const size_t label_pos = prompt.find( "name=\"" + expected + "\"" );
    REQUIRE( label_pos != std::string::npos );
    std::vector<std::string> offered;
    std::string id;
    size_t name_pos = 0;
    while( ( name_pos = prompt.find( " name=\"", name_pos ) ) != std::string::npos ) {
        name_pos += 7;
        const size_t end = prompt.find( '\"', name_pos );
        offered.push_back( prompt.substr( name_pos, end - name_pos ) );
        // The prompt inspection helper collects labels before the real
        // enqueue caller assigns sequential item IDs.
        if( offered.back() == expected ) {
            id = "item_" + std::to_string( offered.size() );
        }
        name_pos = end;
    }
    REQUIRE_FALSE( id.empty() );
    const std::vector<std::string> selected = llm_intent::parse_look_around_response_for_test(
            id + ":1", offered );
    REQUIRE( selected == std::vector<std::string>{ expected } );
    listener.set_llm_intent_item_targets( { { selected.front(), 1 } }, "utf8-pickup" );
    Messages::clear_messages();
    run_npc_turns( listener, 1 );
    CHECK( get_map().i_at( listener.pos_bub() ).empty() );
    int boxes = 0;
    int rations = 0;
    listener.visit_items( [&]( item * node, item * ) {
        if( node->typeId() == itype_id( "box_small" ) ) {
            ++boxes;
            CHECK( node->get_var( "item_label" ) == label );
            CHECK( node->only_item().typeId() == itype_id( "protein_bar_evac" ) );
        }
        rations += node->typeId() == itype_id( "protein_bar_evac" );
        return VisitResponse::NEXT;
    } );
    CHECK( boxes == 1 );
    CHECK( rations == 1 );
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        CHECK( utf32_to_utf8( utf8_to_utf32( message.second ) ) == message.second );
        CHECK( message.second.find( "pickup.item_missing" ) == std::string::npos );
    }
}

TEST_CASE( "native_wear_does_not_leave_nested_items_in_the_old_map_cache",
           "[llm_intent][companion_repair][wear_transfer]" )
{
    setup_snapshot_test_scene();
    map &here = get_map();
    avatar &who = get_avatar();
    who.setpos( here, tripoint_bub_ms( 48, 50, 0 ) );
    item coat( itype_id( "jacket_leather" ), calendar::turn );
    REQUIRE( coat.put_in( item( itype_id( "apple" ), calendar::turn ),
                          pocket_type::CONTAINER ).success() );
    item &placed = here.add_item_or_charges( who.pos_bub(), coat );
    const int64_t apple_uid = placed.only_item().uid().get_value();
    // Use the same cache and exact placed tree as the native submap cache.
    active_item_cache old_cache;
    REQUIRE( old_cache.add( placed, point_sm_ms( 0, 0 ) ) );
    bool was_cached = false;
    for( const item_reference &ref : old_cache.get() ) {
        was_cached |= ref.item_ref && ref.item_ref->uid().get_value() == apple_uid;
    }
    REQUIRE( was_cached );
    const auto result = who.wear( item_location( map_cursor( who.pos_bub() ), &placed ), false );
    REQUIRE( result.has_value() );
    CHECK( ( *result )->only_item().uid().get_value() == apple_uid );
    for( const item_reference &ref : old_cache.get() ) {
        CHECK_FALSE( ( ref.item_ref && ref.item_ref->uid().get_value() == apple_uid ) );
    }
}

TEST_CASE( "native_wear_activity_retains_copy_then_remove_and_refusal_contracts",
           "[llm_intent][companion_repair][wear_transfer]" )
{
    setup_snapshot_test_scene();
    avatar &who = get_avatar();
    const bool wearable = GENERATE( false, true );
    const itype_id type( wearable ? "jacket_leather" : "box_small" );
    item original( type, calendar::turn );
    REQUIRE( original.put_in( item( itype_id( "rock" ), calendar::turn ),
                              pocket_type::CONTAINER ).success() );
    item &placed = get_map().add_item_or_charges( who.pos_bub(), original );
    item_location selected( map_cursor( who.pos_bub() ), &placed );
    const int64_t uid = placed.uid().get_value();
    const int64_t child_uid = placed.only_item().uid().get_value();
    wear_activity_actor actor( { selected }, { 0 } );
    who.set_moves( 10000 );
    actor.do_turn( who.activity, who );
    if( wearable ) {
        CHECK_FALSE( selected );
        CHECK( get_map().i_at( who.pos_bub() ).empty() );
        REQUIRE( who.amount_worn( type ) == 1 );
        const item &worn = **who.worn.top_items_loc( who ).begin();
        CHECK( worn.uid().get_value() != uid );
        CHECK( worn.only_item().uid().get_value() != child_uid );
        CHECK( worn.only_item().typeId() == itype_id( "rock" ) );
    } else {
        REQUIRE( selected );
        CHECK( selected->uid().get_value() == uid );
        CHECK( selected->only_item().uid().get_value() == child_uid );
        CHECK( who.amount_worn( type ) == 0 );
        CHECK( get_map().i_at( who.pos_bub() ).size() == 1 );
    }
}

TEST_CASE( "llm_inventory_wield_then_drop_tracks_the_native_result",
           "[llm_intent][companion_repair][wield_chain]" )
{
    setup_snapshot_test_scene();
    npc &listener = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Listener NPC" );
    listener.set_fac( faction_your_followers );
    const bool copied_descendant = GENERATE( false, true );
    item pack( itype_id( "backpack" ), calendar::turn );
    pack.set_var( "item_label", "chain pack" );
    REQUIRE( pack.put_in( item( itype_id( "rock" ), calendar::turn ),
                          pocket_type::CONTAINER ).success() );
    REQUIRE( listener.wear_item( pack, false ) );
    const std::string offered = llm_intent::build_inventory_prompt_for_test( listener,
        "Wield then drop the rock" );
    const size_t rock = offered.find( "name=\"rock\"" );
    REQUIRE( rock != std::string::npos );
    const size_t id_start = offered.rfind( "id=\"", rock ) + 4;
    const std::string selected = offered.substr( id_start, offered.find( '\"', id_start ) - id_start );
    const size_t pack_name = offered.find( "chain pack" );
    REQUIRE( pack_name != std::string::npos );
    const size_t pack_id_start = offered.rfind( "id=\"", pack_name ) + 4;
    const std::string pack_id = offered.substr( pack_id_start,
                                offered.find( '\"', pack_id_start ) - pack_id_start );
    override_option opt_ui( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    llm_intent::process_inventory_response_for_test( listener,
            "wield: " + ( copied_descendant ? pack_id : selected ) + " | drop: " + selected );
    if( copied_descendant ) {
        // Native wield copies descendant UIDs too.  This remains an explicit
        // chain limitation: refuse the stale child selection safely, never
        // reinterpret it as the container or claim the drop completed.
        REQUIRE( listener.get_wielded_item() );
        CHECK( listener.get_wielded_item()->typeId() == itype_id( "backpack" ) );
        CHECK( listener.get_wielded_item()->only_item().typeId() == itype_id( "rock" ) );
        CHECK( get_map().i_at( listener.pos_bub() ).empty() );
        bool missing = false;
        for( const auto &message : Messages::recent_messages( 0 ) ) {
            missing |= message.second.find( "inventory.item_missing" ) != std::string::npos;
        }
        CHECK( missing );
        return;
    }
    CHECK_FALSE( listener.get_wielded_item() );
    CHECK( listener.worn.top_items_loc( listener ).front()->all_items_top().empty() );
    int rocks = 0;
    for( const item &it : get_map().i_at( listener.pos_bub() ) ) {
        rocks += it.typeId() == itype_id( "rock" );
    }
    CHECK( rocks == 1 );
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        CHECK( message.second.find( "inventory.item_missing" ) == std::string::npos );
    }
}

namespace
{
template<typename Person>
bool equip_gift_fixture( Person &who, item candidate )
{
    return who.wield( candidate );
}

template<typename Person>
std::string gift_character_save( const Person &who )
{
    std::ostringstream saved;
    JsonOut out( saved );
    who.serialize( out );
    return saved.str();
}

bool gift_status_contains( const std::string &expected )
{
    for( const auto &message : Messages::recent_messages( 0 ) ) {
        if( message.second.find( expected ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

item *gift_with_uid( Character &who, int64_t uid )
{
    item *found = nullptr;
    who.visit_items( [&]( item * candidate, item * ) {
        if( candidate->uid().get_value() == uid ) {
            found = candidate;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    return found;
}

void give_turn( npc &donor )
{
    calendar::turn += 1_turns;
    donor.set_moves( 100 );
    donor.move();
}

void select_gift( npc &donor, const std::string &selection = "give: item_1 -> player" )
{
    llm_intent::prepare_inventory_request_for_test( donor, "give-caller-test" );
    llm_intent::process_prepared_inventory_response_for_test( donor, "give-caller-test", selection );
}
}

TEST_CASE( "companion_give_native_pockets_preserve_a_full_stack_without_merging",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    REQUIRE( receiver.wear_item( item( itype_id( "backpack" ), calendar::turn ) ) );
    REQUIRE( equip_gift_fixture( receiver, item( itype_id( "rock" ), calendar::turn ) ) );
    const int64_t weapon_uid = receiver.get_wielded_item()->uid().get_value();
    item existing( itype_id( "9mm" ), calendar::turn, 7 );
    REQUIRE( receiver.try_add( existing, nullptr, nullptr, false ) );
    item ammo( itype_id( "9mm" ), calendar::turn, 61 );
    REQUIRE( donor.wield( ammo ) );
    const int64_t selected_uid = donor.get_wielded_item()->uid().get_value();
    item_location old_location = donor.get_wielded_item();
    select_gift( donor );
    give_turn( donor );
    CHECK( gift_status_contains( "LLM give completed" ) );
    CHECK_FALSE( old_location );
    CHECK_FALSE( donor.get_wielded_item() );
    CHECK( receiver.get_wielded_item()->uid().get_value() == weapon_uid );
    REQUIRE( gift_with_uid( receiver, selected_uid ) );
    CHECK( gift_with_uid( receiver, selected_uid )->charges == 61 );
    int stacks = 0;
    int quantity = 0;
    receiver.visit_items( [&]( item * candidate, item * ) {
        if( candidate->typeId() == itype_id( "9mm" ) ) {
            ++stacks;
            quantity += candidate->charges;
        }
        return VisitResponse::NEXT;
    } );
    CHECK( stacks == 2 );
    CHECK( quantity == 68 );
    CHECK( get_map().i_at( donor.pos_bub() ).empty() );
    CHECK( get_map().i_at( receiver.pos_bub() ).empty() );
}

TEST_CASE( "companion_give_caller_moves_the_exact_container_and_contents_once",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    item box( itype_id( "backpack" ), calendar::turn );
    REQUIRE( box.put_in( item( itype_id( "9mm" ), calendar::turn, 37 ),
                         pocket_type::CONTAINER ).success() );
    REQUIRE( donor.wield( box ) );
    const int64_t root_uid = donor.get_wielded_item()->uid().get_value();
    const int64_t child_uid = donor.get_wielded_item()->only_item().uid().get_value();
    SECTION( "empty hands fallback" ) {}
    SECTION( "compatible pocket with occupied hands" ) {
        REQUIRE( receiver.wear_item( item( itype_id( "backpack" ), calendar::turn ) ) );
        REQUIRE( equip_gift_fixture( receiver, item( itype_id( "rock" ), calendar::turn ) ) );
    }
    select_gift( donor );
    give_turn( donor );
    CHECK( gift_status_contains( "LLM give completed" ) );
    REQUIRE( gift_with_uid( receiver, root_uid ) );
    CHECK( gift_with_uid( receiver, root_uid )->only_item().uid().get_value() == child_uid );
    CHECK( gift_with_uid( receiver, root_uid )->only_item().charges == 37 );
    CHECK_FALSE( gift_with_uid( donor, root_uid ) );
    CHECK_FALSE( donor.get_wielded_item() );
    llm_intent::process_prepared_inventory_response_for_test( donor, "give-caller-test",
            "give: item_1 -> player" ); // the actual response context has been consumed
    std::ostringstream saved;
    JsonOut out( saved );
    receiver.serialize( out );
    auto restored = json_loader::from_string( saved.str() );
    receiver.setID( character_id(), true ); // emulate the fresh-ID native loader
    receiver.deserialize( restored.get_object() );
    give_turn( donor );
    REQUIRE( gift_with_uid( receiver, root_uid ) );
    CHECK( gift_with_uid( receiver, root_uid )->only_item().uid().get_value() == child_uid );
    CHECK( gift_with_uid( receiver, root_uid )->only_item().charges == 37 );
    CHECK_FALSE( gift_with_uid( donor, root_uid ) );
    CHECK( get_map().i_at( donor.pos_bub() ).empty() );
    CHECK( get_map().i_at( receiver.pos_bub() ).empty() );
}

TEST_CASE( "companion_give_refusal_preserves_both_owners_and_full_quantity",
           "[llm_intent][companion_give]" )
{
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    item selected( itype_id( "rock" ), calendar::turn );
    std::string reason = "give.no_space";
    REQUIRE( equip_gift_fixture( receiver, item( itype_id( "rock" ), calendar::turn ) ) );
    SECTION( "occupied hands with no pockets" ) {
    }
    SECTION( "refusal retains a container and nested charge stack" ) {
        selected = item( itype_id( "backpack" ), calendar::turn );
        REQUIRE( selected.put_in( item( itype_id( "9mm" ), calendar::turn, 37 ),
                                  pocket_type::CONTAINER ).success() );
    }
    SECTION( "partial stack capacity cannot silently take a subset" ) {
        REQUIRE( receiver.wear_item( item( itype_id( "jeans" ), calendar::turn ) ) );
        selected = item( itype_id( "9mm" ), calendar::turn, 3000 );
        REQUIRE( receiver.can_stash( item( itype_id( "9mm" ), calendar::turn, 1 ) ) );
        REQUIRE_FALSE( receiver.can_stash( selected ) );
        REQUIRE( receiver.can_pickWeight( selected, true ) );
    }
    SECTION( "native can_wield is not a carrying-weight check" ) {
        receiver.remove_weapon();
        selected = item( itype_id( "anvil_heavy" ), calendar::turn );
        REQUIRE( receiver.can_wield( selected ).success() );
        REQUIRE_FALSE( receiver.can_pickWeight( selected, true ) );
        reason = "give.too_heavy";
    }
    REQUIRE( donor.wield( selected ) );
    const int64_t uid = donor.get_wielded_item()->uid().get_value();
    const int charges = donor.get_wielded_item()->charges;
    const std::string donor_before = llm_intent::gift_tree_identity( *donor.get_wielded_item() );
    const int64_t weapon_uid = receiver.get_wielded_item() ?
                               receiver.get_wielded_item()->uid().get_value() : 0;
    const std::string donor_save = gift_character_save( donor );
    const std::string receiver_save = gift_character_save( receiver );
    auto result = receiver.receive_gift( donor, *donor.get_wielded_item() );
    CHECK_FALSE( result.success() );
    CHECK( ( gift_character_save( donor ) == donor_save ) );
    CHECK( ( gift_character_save( receiver ) == receiver_save ) );
    CHECK( result.str() == reason );
    REQUIRE( donor.get_wielded_item() );
    CHECK( donor.get_wielded_item()->uid().get_value() == uid );
    CHECK( donor.get_wielded_item()->charges == charges );
    CHECK( llm_intent::gift_tree_identity( *donor.get_wielded_item() ) == donor_before );
    CHECK_FALSE( gift_with_uid( receiver, uid ) );
    CHECK( ( receiver.get_wielded_item() ? receiver.get_wielded_item()->uid().get_value() : 0 ) ==
           weapon_uid );
    CHECK( get_map().i_at( donor.pos_bub() ).empty() );
    CHECK( get_map().i_at( receiver.pos_bub() ).empty() );
}

TEST_CASE( "companion_give_uses_listed_recipient_identity_despite_duplicate_names",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    get_avatar().setpos( get_map(), tripoint_bub_ms( 48, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Twin" );
    npc &receiver = spawn_test_npc_at( point_bub_ms( 51, 50 ), "Twin" );
    npc &other = spawn_test_npc_at( point_bub_ms( 50, 51 ), "Twin" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    receiver.set_fac( faction_your_followers );
    other.set_fac( faction_your_followers );
    REQUIRE( equip_gift_fixture( donor, item( itype_id( "rock" ), calendar::turn ) ) );
    const int64_t uid = donor.get_wielded_item()->uid().get_value();
    const std::string handle = "npc_" + std::to_string( receiver.getID().get_value() );
    CHECK( llm_intent::build_inventory_prompt_for_test( donor, "Give to Twin" ).find(
               "id=\"" + handle + "\"" ) != std::string::npos );
    SECTION( "exact eligible ally" ) {
        select_gift( donor, "give: item_1 -> " + handle );
        give_turn( donor );
        CHECK( gift_with_uid( receiver, uid ) );
        CHECK_FALSE( gift_with_uid( other, uid ) );
        CHECK_FALSE( gift_with_uid( donor, uid ) );
    }
    SECTION( "same name does not substitute a changed recipient" ) {
        llm_intent::prepare_inventory_request_for_test( donor, "changed-recipient" );
        receiver.set_fac( faction_id( "no_faction" ) );
        receiver.set_attitude( NPCATT_NULL );
        llm_intent::process_prepared_inventory_response_for_test( donor, "changed-recipient",
                "give: item_1 -> " + handle );
        give_turn( donor );
        CHECK( gift_status_contains( "LLM give blocked: give.recipient_changed" ) );
        CHECK( gift_with_uid( donor, uid ) );
        CHECK_FALSE( gift_with_uid( receiver, uid ) );
        CHECK_FALSE( gift_with_uid( other, uid ) );
    }
    SECTION( "native recipient refusal differs from capacity" ) {
        donor.remove_weapon();
        item poisoned( itype_id( "apple" ), calendar::turn );
        poisoned.poison = 10;
        REQUIRE( donor.wield( poisoned ) );
        receiver.op_of_u.trust = 0;
        REQUIRE_FALSE( receiver.will_accept_from_player( *donor.get_wielded_item() ) );
        const int64_t poison_uid = donor.get_wielded_item()->uid().get_value();
        select_gift( donor, "give: item_1 -> " + handle );
        give_turn( donor );
        CHECK( gift_status_contains( "LLM give blocked: give.refused" ) );
        CHECK( gift_with_uid( donor, poison_uid ) );
        CHECK_FALSE( gift_with_uid( receiver, poison_uid ) );
    }
}

TEST_CASE( "companion_give_stale_and_malformed_selection_never_substitutes_inventory",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    get_avatar().setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    REQUIRE( equip_gift_fixture( donor, item( itype_id( "9mm" ), calendar::turn, 61 ) ) );
    const int64_t uid = donor.get_wielded_item()->uid().get_value();
    llm_intent::prepare_inventory_request_for_test( donor, "stale-give" );
    std::string response = "give: item_1 -> player";
    SECTION( "changed charges" ) {
        donor.get_wielded_item()->charges = 60;
    }
    SECTION( "ownership changed to ground, identical replacement stays put" ) {
        item owned = donor.get_wielded_item().detach();
        get_map().add_item_or_charges( donor.pos_bub(), std::move( owned ) );
        REQUIRE( equip_gift_fixture( donor, item( itype_id( "9mm" ), calendar::turn, 61 ) ) );
    }
    SECTION( "superseding order while model response is pending" ) {
        donor.set_llm_intent_actions( { llm_intent_action::wait_here }, "new-order", "" );
    }
    SECTION( "mixed gift and drop refuses every section" ) {
        response = "give: item_1 -> player | drop: item_1";
    }
    SECTION( "unlisted recipient" ) {
        response = "give: item_1 -> npc_999999";
    }
    SECTION( "duplicate names require a listed item token" ) {
        response = "give: 9mm -> player";
    }
    llm_intent::process_prepared_inventory_response_for_test( donor, "stale-give", response );
    const int64_t current_uid = donor.get_wielded_item()->uid().get_value();
    give_turn( donor );
    CHECK( gift_with_uid( donor, current_uid ) );
    CHECK_FALSE( gift_with_uid( get_avatar(), uid ) );
}

TEST_CASE( "companion_give_new_primary_admission_invalidates_pending_secondary_even_when_primary_fails",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    override_option debug_log( "DEBUG_LLM_INTENT_LOG", "false" );
    // Exercise the actual asynchronous request caller and error response without
    // launching a runner or contacting a provider. This native configuration
    // failure happens before runner.ensure_running.
    override_option backend( "LLM_INTENT_BACKEND", "openvino" );
    override_option api( "LLM_INTENT_USE_API", "false" );
    override_option force_npu( "LLM_INTENT_FORCE_NPU", "true" );
    override_option device( "LLM_INTENT_DEVICE", "CPU" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    get_avatar().setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    REQUIRE( equip_gift_fixture( donor, item( itype_id( "9mm" ), calendar::turn, 61 ) ) );
    const int64_t uid = donor.get_wielded_item()->uid().get_value();
    llm_intent::prepare_inventory_request_for_test( donor, "older-give-secondary" );
    const uint64_t selected_revision = donor.llm_intent_order_revision();
    const bool serial = GENERATE( false, true );
    const bool secondary_after_failure = GENERATE( false, true );
    CAPTURE( serial, secondary_after_failure );
    const auto request = [&]() {
        if( serial ) {
            llm_intent::enqueue_requests( { &donor }, "Wait here instead." );
        } else {
            llm_intent::enqueue_request( donor, "Wait here instead." );
        }
    };
    request();
    CHECK( donor.llm_intent_order_revision() == selected_revision + 1 );
    REQUIRE( llm_intent::has_request_state_for_test( donor, "not-a-request" ) );
    // A rejected duplicate while the accepted request is pending is not another
    // order admission and must not advance the revision.
    const uint64_t accepted_revision = donor.llm_intent_order_revision();
    request();
    CHECK( donor.llm_intent_order_revision() == accepted_revision );
    const auto deliver_old_secondary = [&]() {
        llm_intent::process_prepared_inventory_response_for_test(
            donor, "older-give-secondary", "give: item_1 -> player" );
    };
    if( !secondary_after_failure ) {
        deliver_old_secondary();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
    while( llm_intent::has_request_state_for_test( donor, "not-a-request" ) &&
           std::chrono::steady_clock::now() < deadline ) {
        llm_intent::process_responses();
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
    REQUIRE_FALSE( llm_intent::has_request_state_for_test( donor, "not-a-request" ) );
    CHECK( gift_status_contains( "LLM_INTENT_FORCE_NPU requires device NPU." ) );
    if( secondary_after_failure ) {
        deliver_old_secondary();
    }
    CHECK_FALSE( donor.has_active_llm_action_status() );
    CHECK( donor.llm_intent_order_revision() == accepted_revision );
    give_turn( donor );
    REQUIRE( gift_with_uid( donor, uid ) );
    CHECK( gift_with_uid( donor, uid )->charges == 61 );
    CHECK_FALSE( gift_with_uid( get_avatar(), uid ) );
}

TEST_CASE( "companion_give_native_player_commands_cancel_selection_and_active_approach",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 56, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    auto worn = donor.wear_item( item( itype_id( "backpack" ), calendar::turn ) );
    REQUIRE( worn );
    const int64_t uid = ( **worn ).uid().get_value();
    const bool active_approach = GENERATE( false, true );
    const std::string command = GENERATE( "guard", "stop_guard", "follow", "follow_only" );
    CAPTURE( active_approach, command );
    if( active_approach ) {
        select_gift( donor );
        give_turn( donor );
        REQUIRE( donor.has_active_llm_action_status() );
        REQUIRE( gift_status_contains( "LLM give waiting: give.approaching" ) );
    } else {
        llm_intent::prepare_inventory_request_for_test( donor, "native-command-stale-give" );
    }
    const uint64_t before_command = donor.llm_intent_order_revision();
    if( command == "guard" ) {
        talk_function::assign_guard( donor );
    } else if( command == "stop_guard" ) {
        talk_function::stop_guard( donor );
    } else if( command == "follow" ) {
        talk_function::follow( donor );
    } else {
        talk_function::follow_only( donor );
    }
    CHECK( donor.llm_intent_order_revision() == before_command + 1 );
    if( active_approach ) {
        CHECK( gift_status_contains( "LLM give cancelled: intent.superseded" ) );
    } else {
        llm_intent::process_prepared_inventory_response_for_test(
            donor, "native-command-stale-give", "give: item_1 -> player" );
    }
    CHECK_FALSE( donor.has_active_llm_action_status() );
    // Even an immediately available recipient cannot complete the old gift.
    receiver.setpos( get_map(), donor.pos_bub() + tripoint_rel_ms::east );
    give_turn( donor );
    CHECK( gift_with_uid( donor, uid ) );
    CHECK_FALSE( gift_with_uid( receiver, uid ) );
}

TEST_CASE( "companion_give_native_approach_yields_to_interruption_and_reload",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 56, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    // Keep the selected cargo worn so ordinary danger combat has no reason to
    // discard a poor wielded weapon after the handoff has been interrupted.
    auto worn = donor.wear_item( item( itype_id( "backpack" ), calendar::turn ) );
    REQUIRE( worn );
    const int64_t uid = ( **worn ).uid().get_value();
    select_gift( donor );
    const tripoint_bub_ms initial = donor.pos_bub();
    give_turn( donor );
    CHECK( gift_status_contains( "LLM give waiting: give.approaching" ) );
    REQUIRE( donor.pos_bub() != initial );
    REQUIRE( gift_with_uid( donor, uid ) );
    SECTION( "ordinary reachable handoff" ) {
        for( int turn = 0; turn < 12 && gift_with_uid( donor, uid ); ++turn ) {
            give_turn( donor );
        }
        CHECK( gift_with_uid( receiver, uid ) );
        CHECK( rl_dist( donor.pos_bub(), receiver.pos_bub() ) == 1 );
    }
    SECTION( "superseding command cancels approach" ) {
        donor.set_llm_intent_actions( { llm_intent_action::wait_here }, "stop-give", "" );
        run_npc_turns( donor, 8 );
        CHECK( gift_with_uid( donor, uid ) );
        CHECK_FALSE( gift_with_uid( receiver, uid ) );
    }
    SECTION( "reload cancels an incomplete approach" ) {
        std::ostringstream saved;
        JsonOut out( saved );
        donor.serialize( out );
        auto restored = json_loader::from_string( saved.str() );
        donor.setID( character_id(), true ); // loader assigns the saved identity before cancellation
        donor.deserialize( restored.get_object() );
        receiver.setpos( get_map(), donor.pos_bub() + tripoint( 1, 0, 0 ) );
        give_turn( donor );
        CHECK( gift_with_uid( donor, uid ) );
        CHECK_FALSE( gift_with_uid( receiver, uid ) );
    }
    SECTION( "native unload cancels the approach without transferring cargo" ) {
        donor.on_unload();
        CHECK( gift_status_contains( "LLM give cancelled: give.interrupted_unload" ) );
        CHECK_FALSE( donor.has_active_llm_action_status() );
        CHECK( gift_with_uid( donor, uid ) );
        receiver.setpos( get_map(), donor.pos_bub() + tripoint( 1, 0, 0 ) );
        give_turn( donor );
        CHECK_FALSE( gift_with_uid( receiver, uid ) );
    }
    SECTION( "real hostile contact interrupts without a remote transfer" ) {
        // A weak zombie can be below this NPC's native danger threshold. Use a
        // real adjacent hulk and require the native assessment to recognize it.
        monster &threat = spawn_test_monster( "mon_zombie_hulk", donor.pos_bub() + tripoint( 0, 1, 0 ) );
        REQUIRE( donor.attitude_to( threat ) == Creature::Attitude::HOSTILE );
        REQUIRE( donor.sees( get_map(), threat ) );
        give_turn( donor );
        CAPTURE( donor.get_ai_danger(), donor.danger_assessment(), donor.current_target() != nullptr,
                 donor.has_active_llm_action_status() );
        std::string observed_messages;
        for( const auto &message : Messages::recent_messages( 0 ) ) {
            observed_messages += message.second + "\n";
        }
        CAPTURE( observed_messages );
        REQUIRE( ( donor.get_ai_danger() > 0 || donor.danger_assessment() > 0 || donor.current_target() != nullptr ) );
        CHECK( gift_status_contains( "LLM give blocked: give.danger" ) );
        CHECK( gift_with_uid( donor, uid ) );
        CHECK_FALSE( gift_with_uid( receiver, uid ) );
    }
}

TEST_CASE( "companion_give_duplicate_item_names_select_one_exact_listed_tree",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    get_avatar().setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    item box( itype_id( "backpack" ), calendar::turn );
    // Force separate identical stacks, as an ordinary restored inventory can contain.
    auto pocket = box.get_container_pockets().front();
    REQUIRE( pocket->insert_item( item( itype_id( "9mm" ), calendar::turn, 5 ), false,
                                  false ).success() );
    REQUIRE( pocket->insert_item( item( itype_id( "9mm" ), calendar::turn, 5 ), false,
                                  false ).success() );
    REQUIRE( donor.wield( box ) );
    const auto children = donor.get_wielded_item()->all_items_top();
    REQUIRE( children.size() == 2 );
    const int64_t first_uid = children.front()->uid().get_value();
    const int64_t second_uid = children.back()->uid().get_value();
    REQUIRE( first_uid != second_uid );
    const std::string prompt = llm_intent::build_inventory_prompt_for_test( donor,
        "Give the second stack" );
    CHECK( prompt.find( "id=\"item_3\"" ) != std::string::npos );
    select_gift( donor, "give: item_3 -> player" );
    give_turn( donor );
    CHECK( gift_with_uid( donor, first_uid ) );
    CHECK_FALSE( gift_with_uid( donor, second_uid ) );
    REQUIRE( gift_with_uid( get_avatar(), second_uid ) );
    CHECK( gift_with_uid( get_avatar(), second_uid )->charges == 5 );
}

TEST_CASE( "companion_give_does_not_cross_glass_or_an_unreachable_handoff_ring",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    map &here = get_map();
    avatar &receiver = get_avatar();
    receiver.setpos( here, tripoint_bub_ms( 54, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    REQUIRE( equip_gift_fixture( donor, item( itype_id( "rock" ), calendar::turn ) ) );
    const int64_t uid = donor.get_wielded_item()->uid().get_value();
    for( const tripoint_bub_ms &p : here.points_in_radius( receiver.pos_bub(), 1 ) ) {
        if( p != receiver.pos_bub() ) {
            here.ter_set( p, ter_str_id( "t_wall_glass" ) );
        }
    }
    REQUIRE( donor.sees( here, receiver ) );
    select_gift( donor );
    give_turn( donor );
    CHECK( gift_with_uid( donor, uid ) );
    CHECK_FALSE( gift_with_uid( receiver, uid ) );
    CHECK( gift_status_contains( "LLM give blocked: give.no_path" ) );
    CHECK( donor.pos_bub() == tripoint_bub_ms( 50, 50, 0 ) );
    CHECK( npc::llm_action_bark_for_reason( "give.refused" ) !=
           npc::llm_action_bark_for_reason( "give.no_space" ) );
    CHECK_FALSE( npc::llm_action_bark_for_reason( "give.no_path" ).empty() );
}

TEST_CASE( "companion_give_open_container_never_spills_its_contents",
           "[llm_intent][companion_give]" )
{
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    item bucket( itype_id( "bucket" ), calendar::turn );
    REQUIRE( bucket.put_in( item( itype_id( "water_clean" ), calendar::turn, 5 ),
                            pocket_type::CONTAINER ).success() );
    REQUIRE( donor.wield( bucket ) );
    REQUIRE( donor.get_wielded_item()->will_spill() );
    const int64_t uid = donor.get_wielded_item()->uid().get_value();
    const int64_t child_uid = donor.get_wielded_item()->only_item().uid().get_value();
    auto result = receiver.receive_gift( donor, *donor.get_wielded_item() );
    REQUIRE( result.success() );
    REQUIRE( gift_with_uid( receiver, uid ) );
    CHECK( gift_with_uid( receiver, uid )->only_item().uid().get_value() == child_uid );
    CHECK( gift_with_uid( receiver, uid )->only_item().charges == 5 );
    CHECK( get_map().i_at( donor.pos_bub() ).empty() );
    CHECK( get_map().i_at( receiver.pos_bub() ).empty() );
}

TEST_CASE( "companion_give_binds_loaded_magazine_and_mod_as_one_native_tree",
           "[llm_intent][companion_give]" )
{
    override_option enabled( "LLM_INTENT_ENABLE", "true" );
    override_option debug_status( "DEBUG_LLM_INTENT_UI", "true" );
    Messages::clear_messages();
    setup_snapshot_test_scene();
    avatar &receiver = get_avatar();
    receiver.setpos( get_map(), tripoint_bub_ms( 51, 50, 0 ) );
    npc &donor = spawn_test_npc_at( point_bub_ms( 50, 50 ), "Donor" );
    donor.set_fac( faction_your_followers );
    donor.set_attitude( NPCATT_FOLLOW );
    item gun( itype_id( "glock_19" ), calendar::turn );
    gun.ammo_set( itype_id( "9mm" ), 7 );
    REQUIRE( gun.put_in( item( itype_id( "red_dot_sight" ), calendar::turn ),
                         pocket_type::MOD ).success() );
    REQUIRE( donor.wield( gun ) );
    item_location original = donor.get_wielded_item();
    const int64_t root_uid = original->uid().get_value();
    REQUIRE( original->magazine_current() );
    const int64_t magazine_uid = original->magazine_current()->uid().get_value();
    auto mods = original->all_items_top( pocket_type::MOD );
    REQUIRE( mods.size() == 1 );
    item *mod = mods.front();
    const int64_t mod_uid = mod->uid().get_value();
    mod->set_owner( faction_id( "no_faction" ) );
    auto old_mod_reference = mod->get_safe_reference();
    llm_intent::prepare_inventory_request_for_test( donor, "give-mod-tree" );
    SECTION( "intact full tree and ownership transfer" ) {
        llm_intent::process_prepared_inventory_response_for_test( donor, "give-mod-tree", "give: item_1 -> player" );
        give_turn( donor );
        REQUIRE( receiver.get_wielded_item() );
        CHECK( receiver.get_wielded_item()->uid().get_value() == root_uid );
        CHECK( receiver.get_wielded_item()->magazine_current()->uid().get_value() == magazine_uid );
        CHECK( receiver.get_wielded_item()->ammo_remaining() == 7 );
        auto received_mods = receiver.get_wielded_item()->all_items_top( pocket_type::MOD );
        REQUIRE( received_mods.size() == 1 );
        CHECK( received_mods.front()->uid().get_value() == mod_uid );
        CHECK( received_mods.front()->get_owner() == receiver.get_faction()->id );
        CHECK_FALSE( old_mod_reference );
        CHECK_FALSE( original );
    }
    SECTION( "changed attached mod refuses without substituting its identical name" ) {
        original->remove_item( *mod );
        REQUIRE( original->put_in( item( itype_id( "red_dot_sight" ), calendar::turn ),
                                   pocket_type::MOD ).success() );
        const int64_t replacement_uid = original->all_items_top( pocket_type::MOD ).front()->uid().get_value();
        REQUIRE( replacement_uid != mod_uid );
        llm_intent::process_prepared_inventory_response_for_test( donor, "give-mod-tree", "give: item_1 -> player" );
        give_turn( donor );
        CHECK( gift_status_contains( "LLM give blocked: give.item_changed" ) );
        REQUIRE( donor.get_wielded_item() );
        CHECK( donor.get_wielded_item()->uid().get_value() == root_uid );
        CHECK( donor.get_wielded_item()->magazine_current()->uid().get_value() == magazine_uid );
        CHECK( donor.get_wielded_item()->ammo_remaining() == 7 );
        CHECK( donor.get_wielded_item()->all_items_top( pocket_type::MOD ).front()->uid().get_value() == replacement_uid );
        CHECK_FALSE( receiver.get_wielded_item() );
    }
}
