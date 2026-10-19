#include <algorithm>
#include <clocale>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "cata_catch.h"
#include "cata_scope_helpers.h"
#include "character_id.h"
#include "cursesdef.h"
#include "debug.h"
#include "dialogue.h"
#include "dialogue_win.h"
#include "game.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_helpers.h"
#include "npc.h"
#include "player_helpers.h"
#include "semantic_surface.h"
#include "talker.h"
#include "type_id.h"

namespace
{
const std::string fallback_topic = "TALK_TEST_SEMANTIC_FALLBACK_067";

void load_fallback_topic()
{
    // One ordinary false JSON response leaves the built-in generator to add
    // exactly the same unconditional Bye used by the retained native crash.
    static const bool loaded = []() {
        const JsonObject jo = json_loader::from_string( R"({
            "id":"TALK_TEST_SEMANTIC_FALLBACK_067",
            "dynamic_line":"Take it easy.",
            "responses":[{"text":"Unavailable","condition":{"u_has_trait":"ELFA_EARS"},"topic":"TALK_DONE"}]
        })" );
        load_talk_topic( jo, "test" );
        const JsonObject two = json_loader::from_string( R"({
            "id":"TALK_TEST_SEMANTIC_TWO_067", "dynamic_line":"Take it easy.",
            "replace_built_in_responses":true,
            "responses":[{"text":"Bye.","topic":"TALK_DONE"},
                         {"text":"Another exit.","topic":"TALK_DONE"}]
        })" );
        load_talk_topic( two, "test" );
        const JsonObject disabled = json_loader::from_string( R"({
            "id":"TALK_TEST_SEMANTIC_DISABLED_067", "dynamic_line":"Take it easy.",
            "replace_built_in_responses":true,
            "responses":[{"text":"Unavailable choice","condition":{"u_has_trait":"ELFA_EARS"},
                          "failure_explanation":"Requires a different condition","topic":"TALK_DONE"},
                         {"text":"Bye.","topic":"TALK_DONE"}]
        })" );
        load_talk_topic( disabled, "test" );
        return true;
    }();
    REQUIRE( loaded );
}

void prepare_dialogue( dialogue &d )
{
    clear_avatar();
    get_avatar().unset_mutation( trait_id( "ELFA_EARS" ) );
    clear_map_without_vision();
    map &here = get_map();
    get_avatar().setpos( here, tripoint_bub_ms( 15, 15, 0 ) );
    const character_id id = here.place_npc( point_bub_ms( 25, 25 ),
                                          string_id<npc_template>( "test_talker" ) );
    g->load_npcs();
    npc *speaker = g->find_npc( id );
    REQUIRE( speaker != nullptr );
    d = dialogue( get_talker_for( get_avatar() ), get_talker_for( *speaker ) );
    load_fallback_topic();
}

class dialogue_renderer
{
    public:
        dialogue_renderer() : owns_terminal( !catacurses::stdscr ),
            locale( std::setlocale( LC_CTYPE, nullptr ) ) {
            REQUIRE( std::setlocale( LC_CTYPE, "en_US.UTF-8" ) != nullptr );
            if( owns_terminal ) {
                catacurses::init_interface();
                catacurses::resizeterm();
            }
        }
        ~dialogue_renderer() {
            if( owns_terminal ) {
                catacurses::endwin();
            }
            std::setlocale( LC_CTYPE, locale.c_str() );
        }
    private:
        bool owns_terminal;
        std::string locale;
};
} // namespace

TEST_CASE( "real dialogue Bye callback preserves cached decisions and rechecks predicates",
           "[npc_talk][dialogue_response_067]" )
{
    dialogue_renderer renderer;
    restore_on_out_of_scope<bool> restore_debug( debug_mode );
    debug_mode = false;
    dialogue d;
    prepare_dialogue( d );
    const std::string mode = GENERATE( std::string( "fallback_bye" ),
                                      std::string( "callable_true" ),
                                      std::string( "callable_now_false" ),
                                      std::string( "cached_disabled_no_callable" ),
                                      std::string( "wrong_speaker" ),
                                      std::string( "stale_frame" ),
                                      std::string( "invalid_stable_id" ),
                                      std::string( "missing_stable_id" ),
                                      std::string( "duplicate_stable_id" ) );
    INFO( mode );
    semantic_surface_manager manager( "dialogue-callback-067" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    int publications = 0;
    int predicate_calls = 0;
    bool submitted = false;
    bool canceled_after_rejection = false;
    bool advertised_bye = false;
    manager.set_receipt_observer( [&]( const semantic_action_receipt &receipt ) {
        receipts.push_back( receipt );
        if( !receipt.accepted && !canceled_after_rejection ) {
            canceled_after_rejection = true;
            const auto frame = *manager.top();
            const bool choose_other_exit = mode == "callable_now_false" ||
                                           mode == "cached_disabled_no_callable";
            REQUIRE( manager.submit_request( { manager.run_id(), frame.surface_id, frame.frame_id,
                                               "leave-after-rejection", choose_other_exit ? "dialogue.choose" : "dialogue.cancel",
                                               choose_other_exit ? std::optional<std::string>( d.responses.back().semantic_stable_id ) :
                                               std::nullopt, {} } ) );
            // Drive the actual callback now, before an unrelated raw key read.
            // Selection still returns through opt's ordinary effect/exit path.
            REQUIRE( manager.consume_top_request() );
        }
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor &frame ) {
        if( frame.kind != "dialogue" ) {
            return;
        }
        REQUIRE( ++publications < 8 ); // An unexpected loop fails instead of hanging the runner.
        if( submitted ) {
            return;
        }
        const bool two_rows = mode == "duplicate_stable_id" || mode == "cached_disabled_no_callable" ||
                              mode == "callable_now_false";
        REQUIRE( d.responses.size() == ( two_rows ? 2 : 1 ) );
        if( mode == "duplicate_stable_id" &&
            d.responses.back().semantic_stable_id != d.responses.front().semantic_stable_id ) {
            d.responses.back().semantic_stable_id = d.responses.front().semantic_stable_id;
            return; // Let opt advertise the changed row IDs before requesting that exact frame.
        }
        submitted = true;
        if( mode != "duplicate_stable_id" && mode != "callable_now_false" ) {
            CHECK_FALSE( static_cast<bool>( d.responses[0].condition ) );
        }
        const auto choice = std::find_if( frame.valid_actions.begin(), frame.valid_actions.end(),
        []( const semantic_action_descriptor &action ) {
            return action.id == "dialogue.choose";
        } );
        REQUIRE( choice != frame.valid_actions.end() );
        advertised_bye = choice->enabled;
        std::optional<std::string> selected = choice->stable_id;
        std::string frame_id = frame.frame_id;
        std::map<std::string, std::string> parameters;
        if( mode == "callable_true" || mode == "callable_now_false" ) {
            d.responses[0].condition = [&]( const_dialogue const & ) {
                ++predicate_calls;
                return mode == "callable_true";
            };
        } else if( mode == "wrong_speaker" ) {
            parameters["speaker_id"] = "foreign-speaker";
        } else if( mode == "stale_frame" ) {
            frame_id += "-stale";
        } else if( mode == "invalid_stable_id" ) {
            selected = "old-generated-response";
        } else if( mode == "missing_stable_id" ) {
            selected = std::nullopt;
        } else if( mode == "duplicate_stable_id" ) {
            d.responses.back().semantic_stable_id = *selected;
        }
        REQUIRE( manager.submit_request( { manager.run_id(), frame.surface_id, frame_id,
                                           "choose-bye", "dialogue.choose", selected, parameters } ) );
    } );
    dialogue_window window;
    talk_topic result( "TALK_NONE" );
    const std::string topic = mode == "duplicate_stable_id" || mode == "callable_now_false" ?
                              "TALK_TEST_SEMANTIC_TWO_067" :
                              mode == "cached_disabled_no_callable" ? "TALK_TEST_SEMANTIC_DISABLED_067" :
                              fallback_topic;
    REQUIRE_NOTHROW( result = d.opt( window, talk_topic( topic ) ) );
    CHECK( result.id == "TALK_DONE" );
    semantic_surface_scope successor( manager, "world", "World" );
    CHECK( advertised_bye == ( mode != "cached_disabled_no_callable" ) );
    REQUIRE_FALSE( receipts.empty() );
    const bool accepts_choice = mode == "fallback_bye" || mode == "callable_true";
    CHECK( receipts.front().accepted == accepts_choice );
    if( !accepts_choice ) {
        CHECK( canceled_after_rejection );
        REQUIRE( receipts.size() == 2 );
        CHECK( receipts.back().accepted );
        if( mode == "callable_now_false" || mode == "cached_disabled_no_callable" ) {
            CHECK( receipts.front().rejection_reason == "disabled_stable_id" );
        } else if( mode == "wrong_speaker" ) {
            CHECK( receipts.front().rejection_reason == "wrong_speaker" );
        } else if( mode == "duplicate_stable_id" ) {
            CHECK( receipts.front().rejection_reason == "duplicate_stable_id" );
        }
    }
    CHECK( predicate_calls == ( mode == "callable_true" || mode == "callable_now_false" ? 1 : 0 ) );
}

TEST_CASE( "explained failures and repeated front responses keep their own condition caches",
           "[npc_talk][dialogue_response_067]" )
{
    dialogue d;
    prepare_dialogue( d );
    const JsonObject failed_json = json_loader::from_string( R"({
        "text":"Unavailable choice", "condition":{"u_has_trait":"ELFA_EARS"},
        "failure_explanation":"Requires a different condition", "topic":"TALK_DONE"
    })" );
    json_talk_response failed( failed_json, "test" );
    CHECK_FALSE( failed.gen_responses( d, false ) );
    REQUIRE( d.responses.size() == 1 );
    CHECK_FALSE( static_cast<bool>( d.responses.front().condition ) );
    CHECK( d.response_condition_exists.front() );
    CHECK_FALSE( d.response_condition_eval.front() );
    const std::string failure_id = d.responses.front().semantic_stable_id;

    const JsonObject repeat_json = json_loader::from_string( R"({
        "text":"Repeated choice", "condition":{"not":{"u_has_trait":"ELFA_EARS"}}, "topic":"TALK_DONE"
    })" );
    json_talk_response repeat( repeat_json, "test" );
    repeat.gen_repeat_response( d, itype_id( "rock" ), false );
    REQUIRE( d.responses.size() == 2 );
    CHECK( d.responses.back().semantic_stable_id == failure_id );
    CHECK( d.response_condition_exists.front() );
    CHECK( d.response_condition_eval.front() );
    CHECK( d.response_condition_exists.back() );
    CHECK_FALSE( d.response_condition_eval.back() );

    talk_response bye;
    bye.success.next_topic = talk_topic( "TALK_DONE" );
    d.add_gen_response( bye, false );
    REQUIRE( d.responses.size() == 3 );
    CHECK_FALSE( d.response_condition_eval[1] );
    CHECK( d.response_condition_eval[2] );
    CHECK( d.responses[0].semantic_stable_id != d.responses[1].semantic_stable_id );
    CHECK( d.responses[1].semantic_stable_id != d.responses[2].semantic_stable_id );
}
