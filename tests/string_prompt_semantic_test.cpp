#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "avatar.h"
#include "calendar.h"
#include "cached_options.h"
#include "cata_scope_helpers.h"
#include "cursesdef.h"
#include "game_inventory.h"
#include "item.h"
#include "item_location.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_helpers.h"
#include "player_helpers.h"
#include "sdl_renderer_recovery.h"
#include "type_id.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "imgui/imgui.h"
#include "input_popup.h"
#include "semantic_surface.h"
#include "string_input_popup.h"

namespace
{
void assign_environment( const char *name, const std::optional<std::string> &value )
{
#if defined(_WIN32)
    _putenv_s( name, value ? value->c_str() : "" );
#else
    if( value ) {
        setenv( name, value->c_str(), 1 );
    } else {
        unsetenv( name );
    }
#endif
}

class scoped_imgui_context
{
    public:
        scoped_imgui_context() : owns_context( ImGui::GetCurrentContext() == nullptr ) {
            if( owns_context ) {
                ImGui::CreateContext();
                ImGuiIO &io = ImGui::GetIO();
                io.DisplaySize = ImVec2( 800.0f, 600.0f );
                io.DeltaTime = 1.0f / 60.0f;
                io.Fonts->AddFontDefault();
                io.Fonts->Build();
                ImGui::NewFrame();
            }
        }
        ~scoped_imgui_context() {
            if( owns_context ) {
                ImGui::EndFrame();
                ImGui::DestroyContext();
            }
        }
    private:
        bool owns_context;
};
} // namespace

TEST_CASE( "string prompt environment restores absent and present callers through unwind",
           "[semantic_surface][string_prompt_environment_067]" )
{
    constexpr const char *name = "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH";
    const char *initial = std::getenv( name );
    const std::optional<std::string> saved = initial ? std::optional<std::string>( initial ) :
            std::nullopt;
    on_out_of_scope restore_caller( [&]() {
        assign_environment( name, saved );
    } );
    const auto prior = GENERATE( std::optional<std::string> {},
                                 std::optional<std::string>( "" ),
                                 std::optional<std::string>( "caller-path" ) );
    assign_environment( name, prior );
    const auto exercise = [&]() {
        on_out_of_scope restore_prior( [&]() {
            assign_environment( name, prior );
        } );
        assign_environment( name, "outer-path" );
        REQUIRE( std::getenv( name ) != nullptr );
        CHECK( std::string( std::getenv( name ) ) == "outer-path" );
        {
            on_out_of_scope restore_outer( [&]() {
                assign_environment( name, "outer-path" );
            } );
            assign_environment( name, "inner-path" );
            REQUIRE( std::getenv( name ) != nullptr );
            CHECK( std::string( std::getenv( name ) ) == "inner-path" );
        }
        CHECK( std::string( std::getenv( name ) ) == "outer-path" );
        throw 1;
    };
    CHECK_THROWS_AS( exercise(), int );
#if defined(_WIN32)
    // Windows represents assignment of an empty value as removal.
    const bool restored_present = prior && !prior->empty();
#else
    const bool restored_present = prior.has_value();
#endif
    if( restored_present ) {
        REQUIRE( std::getenv( name ) != nullptr );
        CHECK( std::string( std::getenv( name ) ) == *prior );
    } else {
        CHECK( std::getenv( name ) == nullptr );
    }
}

TEST_CASE( "string prompt applies a semantic result consumed before its owner resumes",
           "[semantic_surface][string_prompt]" )
{
    scoped_imgui_context imgui;
    const bool consumed_before_owner = GENERATE( false, true );
    bool cancel = false;
    SECTION( "submit a quantity" ) {}
    SECTION( "cancel a quantity" ) {
        cancel = true;
    }

    semantic_surface_manager manager( "prompt-run" );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope parent( manager, "world", "World" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    std::string prompt_frame;
    int prompt_descriptor_count = 0;
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "string_prompt" ) {
            return;
        }
        ++prompt_descriptor_count;
        prompt_frame = descriptor.frame_id;
        const std::string action = cancel ? "prompt.cancel" : "prompt.submit";
        REQUIRE( manager.submit_request( { "prompt-run", descriptor.surface_id,
                                           descriptor.frame_id, "quantity", action, std::nullopt,
                                           cancel ? std::map<std::string, std::string>{} :
        std::map<std::string, std::string>{ { "text", "1" } } } ) );
        // input_context::handle_input consumes transport requests itself.
        // Exercise the real popup with exactly that already-consumed state;
        // its local drain must not be required to consume the request twice.
        if( consumed_before_owner ) {
            REQUIRE( manager.consume_top_request() );
            CHECK_FALSE( manager.has_pending_request() );
            CHECK( receipts.empty() );
        }
    } );

    string_input_popup_imgui prompt( 20, "1", "How many?" );
    const std::string quantity = prompt.query();
    if( cancel ) {
        CHECK( quantity == "1" );
        CHECK( prompt.cancelled() );
    } else {
        CHECK( quantity == "1" );
        CHECK_FALSE( prompt.cancelled() );
    }
    REQUIRE( receipts.size() == 1 );
    CHECK( prompt_descriptor_count == 1 );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().requested_frame_id == prompt_frame );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == parent.surface_id() );
    if( !consumed_before_owner ) {
        // The queued route verifies receipt timing.  Reentrant consumption
        // in the descriptor observer above isolates stored-result handling,
        // while publication of that initial descriptor is still in progress.
        CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
    }
}

TEST_CASE( "string prompt rejects malformed text payloads before accepting its bound submission",
           "[semantic_surface][string_prompt]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "prompt-validation-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "string_prompt" ) {
            return;
        }
        REQUIRE( descriptor.valid_actions.size() == 2 );
        CHECK( descriptor.valid_actions[0].id == "prompt.submit" );
        CHECK( descriptor.valid_actions[0].enabled );
        CHECK( descriptor.valid_actions[1].id == "prompt.cancel" );
        CHECK( descriptor.valid_actions[1].enabled );
        REQUIRE( manager.submit_request( { "prompt-validation-run", "wrong-surface", descriptor.frame_id,
                                           "wrong-owner", "prompt.submit", std::nullopt,
        { { "text", "12" } } } ) );
        CHECK_FALSE( manager.consume_top_request() );
        REQUIRE( manager.submit_request( { "prompt-validation-run", descriptor.surface_id, "stale-frame",
                                           "stale-owner", "prompt.submit", std::nullopt,
        { { "text", "12" } } } ) );
        CHECK_FALSE( manager.consume_top_request() );
        REQUIRE( manager.submit_request( { "prompt-validation-run", descriptor.surface_id, descriptor.frame_id,
                                           "missing-text", "prompt.submit", std::nullopt, {} } ) );
        CHECK_FALSE( manager.consume_top_request() );
        REQUIRE( manager.submit_request( { "prompt-validation-run", descriptor.surface_id, descriptor.frame_id,
                                           "extra-field", "prompt.submit", std::nullopt,
        { { "text", "12" }, { "other", "no" } } } ) );
        CHECK_FALSE( manager.consume_top_request() );
        REQUIRE( manager.submit_request( { "prompt-validation-run", descriptor.surface_id, descriptor.frame_id,
                                           "submit", "prompt.submit", std::nullopt,
        { { "text", "12" } } } ) );
    } );

    string_input_popup_imgui prompt( 20, "", "Set hour to?" );
    CHECK( prompt.query() == "12" );
    CHECK_FALSE( prompt.cancelled() );
    // Rejections are acknowledged immediately.  The accepted request is
    // consumed by the prompt owner and demonstrated by its returned value.
    REQUIRE( receipts.size() == 4 );
    CHECK_FALSE( receipts[0].accepted );
    CHECK( receipts[0].rejection_reason == "wrong_surface" );
    CHECK_FALSE( receipts[1].accepted );
    CHECK( receipts[1].rejection_reason == "stale_frame" );
    CHECK_FALSE( receipts[2].accepted );
    CHECK( receipts[2].rejection_reason == "invalid_parameters" );
    CHECK_FALSE( receipts[3].accepted );
    CHECK( receipts[3].rejection_reason == "invalid_parameters" );
}

TEST_CASE( "numeric STRING_INPUT prompt advertises and consumes a semantic submission",
           "[semantic_surface][string_prompt]" )
{
    scoped_imgui_context imgui;
    const bool consumed_by_input_boundary = GENERATE( false, true );
    semantic_surface_manager manager( "numeric-prompt-run" );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope parent( manager, "menu", "Select time point" );
    bool submission_queued = false;
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "string_prompt" ) {
            return;
        }
        CHECK( descriptor.payload.at( "numeric" ) == "true" );
        // The owner publishes once on construction and again at the start of
        // its input loop.  A semantic client submits exactly once against the
        // first currently advertised frame.
        if( submission_queued ) {
            return;
        }
        submission_queued = true;
        REQUIRE( manager.submit_request( { "numeric-prompt-run", descriptor.surface_id,
                                           descriptor.frame_id, "set-hour", "prompt.submit", std::nullopt,
        { { "text", "23" } } } ) );
        if( consumed_by_input_boundary ) {
            // input_context can consume an accepted transport wake before
            // number_input_popup regains its loop.  The popup must still
            // return the submitted native value instead of blocking again.
            REQUIRE( manager.consume_top_request() );
        }
    } );

    number_input_popup<int> prompt( 0, 12, "Set hour to?" );
    CHECK( prompt.query() == 23 );
    CHECK_FALSE( prompt.cancelled() );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == parent.surface_id() );
}

TEST_CASE( "pickup quantity retains its owner across a rejected native input wake",
           "[semantic_surface][pickup][pickup_quantity_owner_067]" )
{
    const bool cancel = GENERATE( false, true );
    const bool preselected = GENERATE( false, true );
    clear_avatar();
    clear_map();
    avatar &you = get_avatar();
    REQUIRE( you.wear_item( item( itype_id( "debug_backpack" ) ), false ) );
    map &here = get_map();
    const tripoint_bub_ms position = you.pos_bub();
    std::string uid;
    std::vector<decltype( item().uid().get_value() )> physical_uids;
    for( int i = 0; i < 3; ++i ) {
        item &placed = here.add_item_or_charges( position, item( itype_id( "2x4" ) ) );
        physical_uids.push_back( placed.uid().get_value() );
        uid = std::to_string( placed.uid().get_value() );
    }
    const time_point turn = calendar::turn;
    const int moves = you.get_moves();
#if defined(TILES)
    software_render_fixture renderer;
    REQUIRE( renderer.available() );
    renderer_recovery_test_support::setup_software_ui();
#else
    if( ImGui::GetCurrentContext() == nullptr ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    }
    on_out_of_scope close_renderer( [&]() {
        catacurses::endwin();
    } );
#endif
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;

    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ( "pickup-quantity-wake-" + std::to_string(
                                               std::chrono::steady_clock::now().time_since_epoch().count() ) + ".jsonl" );
    const char *old_path = std::getenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH" );
    const std::optional<std::string> previous = old_path ?
            std::optional<std::string>( old_path ) : std::nullopt;
    on_out_of_scope cleanup( [&]() {
        assign_environment( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH", previous );
        std::filesystem::remove( path );
    } );
    assign_environment( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH", path.string() );

    semantic_surface_manager manager( "pickup-wake" );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope world( manager, "world", "World" );
    std::optional<semantic_surface_descriptor> prompt;
    int prompt_count = 0;
    int stage = 0;
    int selected_count = -1;
    bool closing_reopened = false;
    bool arrived_inside_input = false;
    int accepted_quantity_receipts = 0;
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        std::cout << "R067_QUANTITY_RECEIPT request=" << receipt.request_id
                  << " accepted=" << receipt.accepted << " owner=" << receipt.consuming_surface_id
                  << "/" << receipt.consuming_frame_id << " reason=" << receipt.rejection_reason << std::endl;
        if( receipt.request_id == "arrival-boundary" ) {
            CHECK_FALSE( receipt.accepted );
            CHECK( receipt.rejection_reason == "wrong_run" );
            // Queue the valid request only after native input has consumed
            // the inert wake. It remains bound to the first advertised owner.
            REQUIRE( prompt );
            arrived_inside_input = true;
            REQUIRE( manager.submit_request( { "pickup-wake", prompt->surface_id,
                                               prompt->frame_id, "quantity", cancel ? "prompt.cancel" : "prompt.submit",
                                               std::nullopt, cancel ? std::map<std::string, std::string>{} :
            std::map<std::string, std::string>{{ "text", "1" }} } ) );
        } else if( receipt.request_id == "quantity" ) {
            CHECK( receipt.accepted );
            if( receipt.accepted ) {
                ++accepted_quantity_receipts;
                if( cancel ) {
                    // Cancel retains the chosen count, so there is no later
                    // changed-payload publication to drive the next action.
                    // Its receipt exposes the freshly restored real parent.
                    REQUIRE( manager.top() );
                    const auto parent = *manager.top();
                    REQUIRE( parent.kind == "inventory" );
                    selected_count = 0;
                    const JsonObject selected = json_loader::from_string( parent.payload.at( "selected_items" ) );
                    for( const JsonMember &member : selected ) {
                        const JsonObject quantity = member.get_object();
                        selected_count += quantity.get_int( "count" );
                        CHECK( quantity.get_string( "unit" ) == "items" );
                    }
                    REQUIRE( manager.submit_request( { manager.run_id(), parent.surface_id, parent.frame_id,
                                                       "finish-cancel", selected_count == 0 ? "inventory.cancel" : "inventory.commit", std::nullopt, {} } ) );
                }
            }
        }
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & descriptor ) {
        std::cout << "R067_QUANTITY_OWNER kind=" << descriptor.kind << " owner=" << descriptor.surface_id
                  << "/" << descriptor.frame_id << std::endl;
        if( descriptor.kind == "string_prompt" ) {
            ++prompt_count;
            if( prompt_count == 1 ) {
                prompt = descriptor;
                CHECK_FALSE( manager.has_pending_request() );
                std::ofstream stream( path );
                stream << "{\"run_id\":\"inert-marker\",\"request_id\":\"arrival-boundary\"}\n";
            } else {
                // Bound the failing source: acknowledge the old request at
                // the real successor (which rejects it), then cancel that
                // successor so the automated counterexample cannot hang.
                if( manager.has_pending_request() ) {
                    CHECK_FALSE( manager.consume_top_request() );
                }
                REQUIRE( manager.submit_request( { "pickup-wake", descriptor.surface_id,
                                                   descriptor.frame_id, "close-reopened", "prompt.cancel", std::nullopt, {} } ) );
            }
            return;
        }
        if( descriptor.kind != "inventory" ) {
            return;
        }
        const JsonObject selected = json_loader::from_string( descriptor.payload.at( "selected_items" ) );
        int count = 0;
        for( const JsonMember &member : selected ) {
            const JsonObject quantity = member.get_object();
            count += quantity.get_int( "count" );
            CHECK( quantity.get_string( "unit" ) == "items" );
        }
        const auto submit = [&]( const std::string & id, const std::string & request_id ) {
            REQUIRE( manager.submit_request( { "pickup-wake", descriptor.surface_id, descriptor.frame_id,
                                               request_id, id, ( id == "inventory.commit" || id == "inventory.cancel" ) ? std::nullopt :
                                               std::optional<std::string>( uid ), {} } ) );
        };
        if( closing_reopened ) {
            return;
        }
        if( prompt_count > 1 ) {
            selected_count = count;
            closing_reopened = true;
            submit( "inventory.cancel", "close-old-selector" );
            return;
        }
        if( stage == 0 ) {
            if( preselected ) {
                ++stage;
                submit( "inventory.toggle", "select-all" );
            } else {
                stage = 2;
                submit( "inventory.set_quantity", "open-quantity" );
            }
        } else if( stage == 1 ) {
            CHECK( count == 3 );
            ++stage;
            submit( "inventory.set_quantity", "open-quantity" );
        } else if( accepted_quantity_receipts == 0 && prompt_count == 1 ) {
            return;
        } else if( count == 3 && !cancel && prompt_count == 1 ) {
            // The restored parent is published before on_input applies 1.
            return;
        } else {
            selected_count = count;
            submit( "inventory.commit", "commit" );
        }
    } );
    const drop_locations selection = game_menus::inv::pickup( { position } );
    CHECK( arrived_inside_input );
    CHECK( accepted_quantity_receipts == 1 );
    CHECK( prompt_count == 1 );
    const int expected_count = cancel ? ( preselected ? 3 : 0 ) : 1;
    CHECK( selected_count == expected_count );
    int total = 0;
    for( const auto &entry : selection ) {
        total += entry.second;
    }
    CHECK( total == expected_count );
    CHECK( here.i_at( position ).size() == 3 );
    std::vector<decltype( item().uid().get_value() )> remaining_uids;
    for( const item &remaining : here.i_at( position ) ) {
        remaining_uids.push_back( remaining.uid().get_value() );
    }
    CHECK( remaining_uids == physical_uids );
    for( const auto &entry : selection ) {
        CHECK( std::find( physical_uids.begin(), physical_uids.end(), entry.first->uid().get_value() ) !=
               physical_uids.end() );
    }
    CHECK( calendar::turn == turn );
    CHECK( you.get_moves() == moves );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == world.surface_id() );
}

#if !defined(TILES)
extern "C" int ungetch( int );
TEST_CASE( "unbound legacy text query still returns after each native editing step",
           "[semantic_surface][pickup_quantity_owner_067]" )
{
    REQUIRE( active_semantic_surface_manager() == nullptr );
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    } else {
        catacurses::refresh();
    }
    on_out_of_scope close_renderer( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    string_input_popup prompt;
    prompt.title( "Native quantity" ).text( "" ).only_digits( true );
    // Construct the real window/context before queuing a key. A missing key
    // fails this disposable control in finite time instead of trapping its runner.
    prompt.query_string( false, true );
    prompt.context().set_timeout( 250 );
    REQUIRE( ungetch( '1' ) != -1 );
    CHECK( prompt.query_string( false ) == "1" );
    CHECK_FALSE( prompt.confirmed() );
    CHECK_FALSE( prompt.canceled() );
    REQUIRE( ungetch( '\n' ) != -1 );
    CHECK( prompt.query_string( false ) == "1" );
    CHECK( prompt.confirmed() );
    CHECK_FALSE( prompt.canceled() );
    CHECK( active_semantic_surface_manager() == nullptr );
}
#endif
