#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "cached_options.h"
#include "avatar.h"
#include "activity_actor_definitions.h"
#include "player_activity.h"
#include "map.h"
#include "map_helpers_tests.h"
#include "map_helpers.h"
#include "do_turn.h"
#include "messages.h"
#include "move_mode.h"
#include <iostream>
#include "npc.h"
#include "monster.h"
#include "options_helpers.h"
#include "worldfactory.h"
#include "effect.h"
#include "coordinates.h"
#include "cata_scope_helpers.h"
#include "character.h"
#include "cursesdef.h"
#include "debug.h"
#include "json.h"
#include "path_info.h"
#include "debug_menu.h"
#include "debug_console.h"
#include "game.h"
#include "imgui/imgui.h"
#include "inventory_ui.h"
#include "input_popup.h"
#include "input_context.h"
#include "sdl_renderer_recovery.h"
#include "item.h"
#include "popup.h"
#include "player_helpers.h"
#include "semantic_surface.h"
#include "raid_decision_trace.h"
#include "string_input_popup.h"
#include "type_id.h"
#include "ui_iteminfo.h"
#include "uilist.h"
#include "ui_manager.h"

TEST_CASE( "redraw debug errors interrupt input without completing its pending action",
           "[semantic_surface][debug_error_prompt_067]" )
{
    const bool enclosing_session = GENERATE( false, true );
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const std::string &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    on_out_of_scope restore_environment( [&]() {
        for( const auto &entry : previous ) {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
    } );
    const std::string run = std::string( enclosing_session ? "redraw-enclosed-" :
                                         "redraw-between-owners-" ) +
                            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    unsetenv( keys[2].c_str() );
#if defined(TILES)
    software_render_fixture renderer;
    REQUIRE( renderer.available() );
    renderer_recovery_test_support::setup_software_ui();
#else
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    }
    on_out_of_scope close_renderer( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
#endif
    semantic_surface_manager local_manager( run );
    semantic_surface_manager &manager = enclosing_session ? local_manager :
                                        openclaw_harness_semantic_surface_manager();
    on_out_of_scope clear_observers( [&]() {
        manager.set_descriptor_observer( {} );
        manager.set_receipt_observer( {} );
    } );
    std::optional<semantic_surface_manager_session> session;
    if( enclosing_session ) {
        session.emplace( manager );
    }
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    int original_dispatches = 0;
    {
        semantic_surface_scope world( manager, "world", "World", {},
        { { "world.move.north", "", "North", true } },
        [&]( const semantic_action_request & request ) {
            ++original_dispatches;
            REQUIRE( manager.withhold_parent_authority_until_recreated( request.surface_id ) );
            return semantic_action_dispatch_result{ true, "", "" };
        } );
        const auto owner = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, owner.frame_id,
                                           "north", "world.move.north", std::nullopt, {} } ) );
        REQUIRE( world.consume_request() );
    }
    REQUIRE_FALSE( manager.top() );
    REQUIRE( receipts.empty() );
    std::vector<semantic_surface_descriptor> prompts;
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & owner ) {
        if( owner.payload.count( "native_owner" ) == 0 ||
            owner.payload.at( "native_owner" ) != "DEBUG_MSG" ) {
            return;
        }
        prompts.push_back( owner );
        CHECK( active_semantic_surface_manager() == &manager );
        CHECK( std::none_of( receipts.begin(), receipts.end(), []( const auto & receipt ) {
            return receipt.request_id == "north";
        } ) );
        CHECK( owner.payload.at( "text" ).find( "controlled redraw error" ) != std::string::npos );
        CHECK( owner.valid_actions.size() == 1 );
        const auto suffix = std::to_string( prompts.size() );
        REQUIRE( manager.submit_request( { "foreign-run", owner.surface_id, owner.frame_id,
                                           "foreign-continue" + suffix, "prompt.acknowledge", std::nullopt, {} } ) );
        REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, owner.frame_id,
                                           "parameter-continue" + suffix, "prompt.acknowledge", std::nullopt,
        { { "ignore_future_errors", "true" } } } ) );
        REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, "stale-frame",
                                           "stale-continue" + suffix, "prompt.acknowledge", std::nullopt, {} } ) );
        REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, owner.frame_id,
                                           "continue" + suffix, "prompt.acknowledge", std::nullopt, {} } ) );
        // Once a real semantic owner exists, exercise its native redraw too.
        test_mode = false;
    } );

    // The public buffer producer lets the actual redraw-safe prompt run under
    // the test input guard: old raw-only code fails instead of waiting on keys.
    {
        restore_on_out_of_scope<catacurses::window> restore_screen( catacurses::stdscr );
        restore_on_out_of_scope<bool> restore_mode( test_mode );
        catacurses::stdscr = catacurses::window();
        test_mode = false;
        for( int repeat = 0; repeat < 2; ++repeat ) {
            CHECK( capture_debugmsg_during( [&]() {
                realDebugmsg( "redraw-control.cpp", "1", "redraw_control", "controlled redraw error" );
            } ) == "controlled redraw error" );
        }
    }
    bool invoked = false;
    ui_adaptor drawing;
    drawing.position_from_window( catacurses::stdscr );
    drawing.on_redraw( [&]( const ui_adaptor & ) {
        if( !invoked ) {
            invoked = true;
            restore_on_out_of_scope<bool> input_guard( test_mode );
            test_mode = true;
            replay_buffered_debugmsg_prompts();
        }
    } );
    restore_on_out_of_scope<bool> restore_mode( test_mode );
    test_mode = false;
    REQUIRE_NOTHROW( ui_manager::redraw() );
    REQUIRE( invoked );
    // Continue never grants future Ignore: the second identical error still
    // needs its own current prompt and acknowledgement.
    REQUIRE( prompts.size() == 2 );
    REQUIRE( receipts.size() == 8 );
    for( std::size_t offset : {
             0, 4
         } ) {
        CHECK_FALSE( receipts[offset].accepted );
        CHECK( receipts[offset].rejection_reason == "wrong_run" );
        CHECK_FALSE( receipts[offset + 1].accepted );
        CHECK( receipts[offset + 1].rejection_reason == "unadvertised_action" );
        CHECK_FALSE( receipts[offset + 2].accepted );
        CHECK( receipts[offset + 2].rejection_reason == "stale_frame" );
        CHECK( receipts[offset + 3].accepted );
        CHECK( receipts[offset + 3].outcome == "debug_error_continued" );
        CHECK( receipts[offset + 3].resulting_frame_id.empty() );
    }
    REQUIRE_FALSE( manager.top() );
    CHECK( active_semantic_surface_manager() == ( enclosing_session ? &manager : nullptr ) );
    {
        semantic_surface_scope successor( manager, "world", "World after redraw" );
        REQUIRE( receipts.size() == 9 );
        CHECK( receipts.back().request_id == "north" );
        CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
        CHECK( receipts.back().resulting_frame_id != prompts[0].frame_id );
        CHECK( original_dispatches == 1 );
        const auto original_receipt = manager.reject_request( { manager.run_id(), "", "",
                                      "north", "world.move.north", std::nullopt, {} } );
        CHECK( original_receipt.resulting_frame_id == receipts.back().resulting_frame_id );
        REQUIRE( manager.submit_request( { manager.run_id(), prompts.back().surface_id,
                                           prompts.back().frame_id, "retired-prompt", "prompt.acknowledge", std::nullopt, {} } ) );
        CHECK_FALSE( successor.consume_request() );
        CHECK( receipts.back().rejection_reason == "wrong_surface" );
        CHECK( original_dispatches == 1 );
    }
}

TEST_CASE( "diagnostic scopes preserve pending child ownership across nested owners",
           "[semantic_surface][debug_error_prompt_067]" )
{
    const bool withheld = GENERATE( false, true );
    semantic_surface_manager manager( "interrupted-child" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const auto & receipt ) {
        receipts.push_back( receipt );
    } );
    int parent_dispatches = 0;
    int acknowledgements = 0;
    semantic_surface_scope parent( manager, "inventory", "Inventory", {},
    { { "inventory.select", "item:1", "Select", true } },
    [&]( const semantic_action_request & request ) {
        ++parent_dispatches;
        if( withheld ) {
            REQUIRE( manager.withhold_parent_authority_until_recreated( request.surface_id ) );
        }
        return semantic_action_dispatch_result{ true, "", "", true };
    } );
    const auto parent_owner = *manager.top();
    REQUIRE( manager.submit_request( { manager.run_id(), parent_owner.surface_id, parent_owner.frame_id,
                                       "select", "inventory.select", "item:1", {} } ) );
    REQUIRE( parent.consume_request() );
    REQUIRE( receipts.empty() );
    {
        semantic_surface_scope diagnostic( manager, "prompt", "Debug error", {},
        { { "prompt.acknowledge", "", "Continue", true } },
        [&]( const semantic_action_request & ) {
            ++acknowledgements;
            return semantic_action_dispatch_result{ true, "", "", false, false };
        }, true );
        REQUIRE( receipts.empty() );
        const auto diagnostic_owner = *manager.top();
        {
            semantic_surface_scope child( manager, "menu", "Nested owner" );
            const auto child_owner = *manager.top();
            CHECK_FALSE( diagnostic.close() );
            REQUIRE( manager.top() );
            CHECK( manager.top()->surface_id == child_owner.surface_id );
            CHECK( manager.top()->frame_id == child_owner.frame_id );
            REQUIRE( manager.submit_request( { manager.run_id(), diagnostic_owner.surface_id,
                                               diagnostic_owner.frame_id, "parent-while-child", "prompt.acknowledge", std::nullopt, {} } ) );
            CHECK_FALSE( diagnostic.consume_request() );
            CHECK_FALSE( child.consume_request() );
            CHECK( receipts.back().rejection_reason == "wrong_surface" );
        }
        const auto fresh_diagnostic = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), fresh_diagnostic.surface_id,
                                           fresh_diagnostic.frame_id, "ack", "prompt.acknowledge", std::nullopt, {} } ) );
        REQUIRE( diagnostic.consume_request() );
        CHECK( acknowledgements == 1 );
        CHECK( receipts.size() == 2 );
        // Move ownership once; the moved-from scope must not restore twice.
        semantic_surface_scope moved( std::move( diagnostic ) );
        REQUIRE( moved.close() );
    }
    CHECK( receipts.size() == 2 );
    if( withheld ) {
        REQUIRE_FALSE( manager.top() );
    } else {
        REQUIRE( manager.top() );
        CHECK( manager.top()->frame_id == parent_owner.frame_id );
        CHECK( parent.publish( { { "redraw", "true" } },
        { { "inventory.select", "item:1", "Select", true } } ) );
        CHECK( receipts.size() == 2 );
    }
    {
        semantic_surface_scope real_successor( manager, "menu", "Actual selection child" );
        REQUIRE( receipts.size() == 3 );
        CHECK( receipts.back().request_id == "select" );
        CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
        CHECK( parent_dispatches == 1 );
    }
}

TEST_CASE( "activity input survives polling but not a different owner",
           "[semantic_surface][activity_poll]" )
{
    semantic_surface_manager manager( "activity-run" );
    int pauses = 0;
    const auto consumer = [&]( const semantic_action_request & ) {
        ++pauses;
        return semantic_action_dispatch_result{ true, "", "", false, false };
    };
    const auto poll = [&]( const std::string & generation ) {
        return manager.push( "activity_wait", "Activity in progress",
        { { "activity_generation", generation } },
        { { "activity.pause", "", "Pause activity", true } }, consumer );
    };
    std::string surface = poll( "1" );
    const semantic_surface_descriptor observed = *manager.top();
    REQUIRE( manager.pop( surface ) );
    surface = poll( "1" );
    CHECK( manager.top()->frame_id == observed.frame_id );
    REQUIRE( manager.submit_request( { observed.run_id, observed.surface_id, observed.frame_id,
                                       "pause-1", "activity.pause", std::nullopt, {} } ) );
    CHECK( manager.consume_top_request() );
    CHECK( pauses == 1 );
    REQUIRE( manager.pop( surface ) );

    SECTION( "a replacement activity rejects the previous grant" ) {
        poll( "2" );
    }
    SECTION( "a prompt retires the grant even if the activity resumes" ) {
        const std::string prompt = manager.push( "yes_no", "Stop?" );
        REQUIRE( manager.pop( prompt ) );
        poll( "1" );
    }
    SECTION( "a withheld activity unwinds after its cancellation prompt" ) {
        const std::string activity = poll( "1" );
        REQUIRE( manager.withhold_parent_authority_until_recreated( activity ) );
        const std::string prompt = manager.push( "yes_no", "Stop?" );
        REQUIRE( manager.pop( prompt ) );
        REQUIRE_FALSE( manager.top() );
        REQUIRE( manager.pop( activity ) );
        poll( "1" );
    }
    CHECK( manager.top()->frame_id != observed.frame_id );
    REQUIRE( manager.submit_request( { observed.run_id, observed.surface_id, observed.frame_id,
                                       "stale-pause", "activity.pause", std::nullopt, {} } ) );
    CHECK_FALSE( manager.consume_top_request() );
    CHECK( pauses == 1 );
}

TEST_CASE( "failed diagnostic construction restores the pending action and native stack",
           "[semantic_surface][diagnostic_constructor_exception_067]" )
{
    semantic_surface_manager manager( "failed-diagnostic" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const auto & receipt ) {
        receipts.push_back( receipt );
    } );
    int dispatches = 0;
    {
        semantic_surface_scope world( manager, "world", "World", {},
        { { "world.move.north", "", "North", true } },
        [&]( const semantic_action_request & request ) {
            ++dispatches;
            REQUIRE( manager.withhold_parent_authority_until_recreated( request.surface_id ) );
            return semantic_action_dispatch_result{ true, "", "" };
        } );
        const auto owner = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, owner.frame_id,
                                           "north", "world.move.north", std::nullopt, {} } ) );
        REQUIRE( world.consume_request() );
    }
    manager.set_descriptor_observer( []( const auto & ) {
        throw std::runtime_error( "descriptor publication failed" );
    } );
    CHECK_THROWS_WITH( semantic_surface_scope( manager, "prompt", "Debug error", {}, {}, {}, true ),
                       "descriptor publication failed" );
    CHECK( manager.stack().empty() );
    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );
    manager.set_descriptor_observer( {} );
    {
        semantic_surface_scope successor( manager, "world", "Actual successor" );
        CHECK( receipts.size() == 1 );
        if( !receipts.empty() ) {
            CHECK( receipts.back().request_id == "north" );
            CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
        }
        CHECK( dispatches == 1 );
    }
    // Also clean the original implementation's partially pushed orphan after
    // recording the failed assertions, so the negative control stays bounded.
    while( !manager.stack().empty() ) {
        const auto owners = manager.stack();
        REQUIRE( manager.pop( owners.back().surface_id ) );
    }
}

TEST_CASE( "diagnostic unwinding never publishes or throws from its destructor",
           "[semantic_surface][diagnostic_destructor_exception_067]" )
{
    const bool unwinding = GENERATE( false, true );
    semantic_surface_manager manager( "diagnostic-unwind" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const auto & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope parent( manager, "world", "World", {},
    { { "world.move.north", "", "North", true } }, []( const auto & ) {
        return semantic_action_dispatch_result{ true, "", "" };
    } );
    const auto owner = *manager.top();
    REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, owner.frame_id,
                                       "north", "world.move.north", std::nullopt, {} } ) );
    REQUIRE( parent.consume_request() );
    // The broken noexcept destructor would terminate the negative runner.
    // Keep that a truthful failed exit without producing a core/crash archive.
    const auto previous_terminate = std::set_terminate( []() {
        std::fputs( "FAILED: diagnostic destructor propagated observer exception\n", stderr );
        std::fflush( stderr );
        std::_Exit( 86 );
    } );
    on_out_of_scope restore_terminate( [&]() {
        std::set_terminate( previous_terminate );
    } );
    const auto native_body = [&]() {
        semantic_surface_scope diagnostic( manager, "prompt", "Debug error", {}, {}, {}, true );
        manager.set_descriptor_observer( []( const auto & ) {
            throw std::runtime_error( "parent publication failed" );
        } );
        // The caller is unwinding its native input/render operation. Cleanup
        // must not run another potentially throwing external observer.
        if( unwinding ) {
            throw std::runtime_error( "native input interrupted" );
        }
    };
    if( unwinding ) {
        CHECK_THROWS_WITH( native_body(), "native input interrupted" );
    } else {
        native_body();
    }
    manager.set_descriptor_observer( {} );
    CHECK( manager.stack().size() == 1 );
    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );
    {
        semantic_surface_scope successor( manager, "world", "Actual successor" );
        REQUIRE( receipts.size() == 1 );
        CHECK( receipts.back().request_id == "north" );
        CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
    }
}

TEST_CASE( "failed diagnostic close restores custody before the native owner retries publication",
           "[semantic_surface][diagnostic_close_exception_067]" )
{
    semantic_surface_manager manager( "failed-close" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const auto & receipt ) {
        receipts.push_back( receipt );
    } );
    int dispatches = 0;
    semantic_surface_scope parent( manager, "inventory", "Inventory", {},
    { { "inventory.select", "item:1", "Select", true } }, [&]( const auto & ) {
        ++dispatches;
        return semantic_action_dispatch_result{ true, "", "", dispatches == 1 };
    } );
    const auto owner = *manager.top();
    REQUIRE( manager.submit_request( { manager.run_id(), owner.surface_id, owner.frame_id,
                                       "select", "inventory.select", "item:1", {} } ) );
    REQUIRE( parent.consume_request() );
    std::optional<semantic_surface_descriptor> failed_owner;
    manager.set_descriptor_observer( [&]( const auto & descriptor ) {
        failed_owner = descriptor;
        throw std::runtime_error( "construction observer failed" );
    } );
    CHECK_THROWS_WITH( semantic_surface_scope( manager, "prompt", "Debug error", {}, {}, {}, true ),
                       "construction observer failed" );
    REQUIRE( failed_owner );
    REQUIRE( manager.stack().size() == 1 );
    CHECK( manager.stack().front().surface_id == owner.surface_id );
    REQUIRE_FALSE( manager.top() );
    REQUIRE( manager.submit_request( { manager.run_id(), failed_owner->surface_id, failed_owner->frame_id,
                                       "failed-prompt", "prompt.acknowledge", std::nullopt, {} } ) );
    CHECK_FALSE( parent.consume_request() );
    CHECK( receipts.empty() );
    manager.set_descriptor_observer( {} );
    REQUIRE( parent.publish( {}, owner.valid_actions ) );
    CHECK_FALSE( parent.consume_request() );
    CHECK( receipts.back().rejection_reason == "wrong_surface" );
    CHECK( dispatches == 1 );
    {
        semantic_surface_scope child( manager, "menu", "Actual first selection child" );
        REQUIRE( receipts.back().request_id == "select" );
        CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
    }
    // Complete the awaited child before exercising an ordinary in-place
    // action. Await-child suppression must not be waived just to throw.
    const std::map<std::string, std::string> current_facts = { { "stage", "after-child" } };
    REQUIRE( parent.publish( current_facts, owner.valid_actions ) );
    const auto ready_parent = *manager.top();
    REQUIRE( manager.submit_request( { manager.run_id(), ready_parent.surface_id, ready_parent.frame_id,
                                       "second-select", "inventory.select", "item:1", {} } ) );
    REQUIRE( parent.consume_request() );
    CHECK( dispatches == 2 );
    {
        semantic_surface_scope diagnostic( manager, "prompt", "Debug error", {},
        { { "prompt.acknowledge", "", "Continue", true } }, []( const auto & ) {
            return semantic_action_dispatch_result{ true, "", "", false, false };
        }, true );
        const auto prompt = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), prompt.surface_id, prompt.frame_id,
                                           "continue", "prompt.acknowledge", std::nullopt, {} } ) );
        REQUIRE( diagnostic.consume_request() );
        manager.set_descriptor_observer( [&]( const auto & descriptor ) {
            failed_owner = descriptor;
            throw std::runtime_error( "parent observer failed" );
        } );
        CHECK_THROWS_WITH( diagnostic.close(), "parent observer failed" );
        REQUIRE( manager.stack().size() == 1 );
        CHECK( manager.stack().front().surface_id == owner.surface_id );
        REQUIRE_FALSE( manager.top() );
        CHECK( diagnostic.close() );
    }
    REQUIRE( failed_owner );
    REQUIRE( manager.submit_request( { manager.run_id(), failed_owner->surface_id, failed_owner->frame_id,
                                       "failed-parent", "inventory.select", "item:1", {} } ) );
    CHECK_FALSE( parent.consume_request() );
    manager.set_descriptor_observer( {} );
    // Even identical native facts need a fresh descriptor after failed output.
    REQUIRE( parent.publish( current_facts, owner.valid_actions ) );
    REQUIRE( manager.top() );
    CHECK( manager.top()->frame_id != failed_owner->frame_id );
    CHECK_FALSE( parent.consume_request() );
    CHECK( receipts.back().rejection_reason == "stale_frame" );
    CHECK( dispatches == 2 );
    for( const auto &request_id : {
             "select", "second-select"
         } ) {
        CHECK( std::count_if( receipts.begin(), receipts.end(), [&]( const auto & receipt ) {
            return receipt.request_id == request_id;
        } ) == 1 );
    }
    const auto completed = manager.reject_request( { manager.run_id(), ready_parent.surface_id,
                           ready_parent.frame_id, "second-select", "inventory.select", "item:1", {} } );
    CHECK( completed.resulting_frame_id == manager.top()->frame_id );
    CHECK( dispatches == 2 );
}

namespace
{

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

class test_inventory_selector : public inventory_selector
{
    public:
        using inventory_selector::handle_semantic_request;
        using inventory_selector::semantic_actions;

        explicit test_inventory_selector( Character &who ) : inventory_selector( who ) {}
};

} // namespace

TEST_CASE( "DebugConsole advertises native close and restores its input parent once",
           "[semantic_surface][debug_console_close_067]" )
{
    scoped_imgui_context imgui;
    // A semantic close is an explicit owner command, including when the
    // console's editor currently owns ordinary keyboard text input.
    restore_on_out_of_scope<bool> typing( ImGui::GetIO().WantTextInput );
    ImGui::GetIO().WantTextInput = true;
    semantic_surface_manager manager( "console-run" );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope world( manager, "world", "World" );
    const auto before = *manager.top();
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    bool opened = false;
    semantic_action_request close;
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind == "world" || opened ) {
            return;
        }
        opened = true;
        REQUIRE( descriptor.payload.at( "native_owner" ) == "DEBUG_CONSOLE" );
        REQUIRE( descriptor.valid_actions.size() == 1 );
        REQUIRE( descriptor.valid_actions.front().id == "debug_console.close" );
        close = { descriptor.run_id, descriptor.surface_id, descriptor.frame_id,
                  "close-console", "debug_console.close", std::nullopt, {}
                };
        auto wrong = close;
        wrong.request_id = "wrong-console";
        wrong.surface_id = before.surface_id;
        REQUIRE( manager.submit_request( wrong ) );
        CHECK_FALSE( manager.consume_top_request() );
        CHECK( manager.top()->surface_id == descriptor.surface_id );
        {
            semantic_surface_scope child( manager, "string_prompt", "Console editor" );
            auto parent_request = close;
            parent_request.request_id = "parent-while-editor-open";
            REQUIRE( manager.submit_request( parent_request ) );
            CHECK_FALSE( manager.consume_top_request() );
            CHECK( manager.top()->kind == "string_prompt" );
        }
        auto stale = close;
        stale.request_id = "old-console-frame";
        REQUIRE( manager.submit_request( stale ) );
        CHECK_FALSE( manager.consume_top_request() );
        close.frame_id = manager.top()->frame_id;
        REQUIRE( manager.submit_request( close ) );
    } );
    debug_menu::open_console();
    REQUIRE( opened );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == before.surface_id );
    CHECK( manager.top()->frame_id != before.frame_id );
    REQUIRE( receipts.size() == 4 );
    CHECK_FALSE( receipts.front().accepted );
    CHECK( receipts.front().rejection_reason == "wrong_surface" );
    CHECK( receipts[1].rejection_reason == "wrong_surface" );
    CHECK( receipts[2].rejection_reason == "stale_frame" );
    CHECK( receipts.back().accepted );
    CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
    CHECK_FALSE( manager.submit_request( close ) );
    CHECK_FALSE( manager.consume_top_request() );
    CHECK( manager.top()->kind == "world" );
    CHECK( receipts.size() == 5 );
    CHECK( receipts.back().accepted ); // Immutable receipt replay, no new close.
    CHECK( receipts.back().resulting_frame_id == receipts[3].resulting_frame_id );
}

TEST_CASE( "native compatibility observations do not duplicate input-owner render maps",
           "[semantic_surface][lean_native_observation_067]" )
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() /
                               ( "lean-native-observation-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( directory );
    const fs::path trace = directory / "native.jsonl";
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_TRACE_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    on_out_of_scope cleanup( [&] {
        for( const auto &entry : previous )
        {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
        fs::remove_all( directory );
    } );
    const auto run = directory.filename().string();
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    setenv( keys[2].c_str(), trace.string().c_str(), 1 );
    const auto context = get_default_mode_input_context();
    openclaw_harness_semantic_initial_world_frame_if_ready( &context, true, true, true );
    auto &manager = openclaw_harness_semantic_surface_manager();
    // Publish the actual native HUD maps once through the input-owning
    // descriptor. The callback is a controlled transaction, not gameplay.
    std::map<std::string, std::string> payload;
    {
        std::ifstream initial( trace );
        std::string row;
        const std::string prefix = "openclaw_harness_semantic_step: ";
        while( std::getline( initial, row ) ) {
            std::istringstream body( row.substr( prefix.size() ) );
            TextJsonIn jsin( body );
            const auto object = jsin.get_object();
            object.allow_omitted_members();
            if( object.get_string( "event" ) == "frame" ) {
                const auto observation = object.get_object( "observation" );
                observation.allow_omitted_members();
                payload["minimap"] = observation.get_object( "minimap" ).str();
                payload["overmap"] = observation.get_object( "overmap" ).str();
                payload["visible_local"] = observation.get_array( "visible_local" ).str();
                payload["messages"] = "[]";
            }
        }
    }
    REQUIRE( payload.size() == 4 );
    int actions = 0;
    semantic_surface_scope world( manager, "world", "World", payload,
    { { "world.pause", "", "Pause", true } }, [&]( const semantic_action_request & ) {
        ++actions;
        return semantic_action_dispatch_result{ true, "", "", false, false };
    } );
    const auto issued = *manager.top();
    REQUIRE( manager.submit_request( { issued.run_id, issued.surface_id,
                                       issued.frame_id, "pause", "world.pause", std::nullopt, {} } ) );
    REQUIRE( world.consume_request() );
    CHECK( actions == 1 );
    for( int index = 0; index < 3; ++index ) {
        openclaw_harness_semantic_activity_distraction();
        openclaw_harness_semantic_world_after_activity_distraction();
        openclaw_harness_semantic_wait_activity_complete();
    }
    std::ifstream input( trace );
    const std::string prefix = "openclaw_harness_semantic_step: ";
    std::string line;
    int startup = 0;
    int activity = 0;
    int receipts = 0;
    int descriptors = 0;
    std::size_t startup_bytes = 0;
    std::size_t activity_bytes = 0;
    while( std::getline( input, line ) ) {
        REQUIRE( line.compare( 0, prefix.size(), prefix ) == 0 );
        std::istringstream body( line.substr( prefix.size() ) );
        TextJsonIn jsin( body );
        const auto object = jsin.get_object();
        object.allow_omitted_members();
        if( object.get_string( "event" ) == "surface_receipt" ) {
            CHECK( object.get_bool( "accepted" ) );
            CHECK( object.get_string( "request_id" ) == "pause" );
            ++receipts;
        }
        if( object.get_string( "event" ) == "surface_descriptor" ) {
            const auto published = object.get_object( "payload" );
            published.allow_omitted_members();
            CHECK( published.get_string( "minimap" ) == payload.at( "minimap" ) );
            CHECK( published.get_string( "overmap" ) == payload.at( "overmap" ) );
            CHECK( object.get_string( "frame_id" ) == issued.frame_id );
            ++descriptors;
        }
        if( object.get_string( "event" ) != "frame" ) {
            continue;
        }
        const auto observation = object.get_object( "observation" );
        observation.allow_omitted_members();
        CHECK( observation.has_member( "visible_local" ) );
        CHECK( observation.has_member( "visible_entities" ) );
        CHECK( observation.has_member( "visible_zones" ) );
        if( object.get_string( "producer", "" ) == "hud_world_ready" ) {
            ++startup;
            startup_bytes += line.size() + 1;
            CHECK( observation.has_member( "minimap" ) );
            CHECK( observation.has_member( "overmap" ) );
        } else {
            ++activity;
            activity_bytes += line.size() + 1;
            CHECK_FALSE( observation.has_member( "minimap" ) );
            CHECK_FALSE( observation.has_member( "overmap" ) );
        }
    }
    CHECK( startup == 1 );
    CHECK( activity == 9 );
    CHECK( receipts == 1 );
    CHECK( descriptors == 1 );
    std::cout << "R067_NATIVE_OBSERVATION startup_bytes=" << startup_bytes
              << " activity_records=" << activity << " activity_bytes=" << activity_bytes
              << " persisted_bytes=" << fs::file_size( trace ) << std::endl;
}

TEST_CASE( "semantic surface scopes hide parent actions and republish after pop",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    semantic_surface_scope world( manager, "world", "World", {}, {
        { "world.wait", "", "Wait", true }
    } );
    REQUIRE( manager.top() );
    const std::string world_surface = manager.top()->surface_id;
    const std::string world_frame = manager.top()->frame_id;

    {
        semantic_surface_scope child( manager, "menu", "Inventory", {}, {
            { "menu.cancel", "", "Cancel", true }
        } );
        REQUIRE( manager.top() );
        CHECK( manager.top()->kind == "menu" );
        CHECK( manager.top()->breadcrumbs == std::vector<std::string> { "World", "Inventory" } );
        CHECK( manager.top()->valid_actions.front().id == "menu.cancel" );
        CHECK_FALSE( world.publish( {}, { { "world.wait", "", "Wait", true } } ) );
        CHECK( manager.stack().size() == 2 );
    }

    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == world_surface );
    CHECK( manager.top()->frame_id != world_frame );
    CHECK( manager.top()->valid_actions.front().id == "world.wait" );
}

TEST_CASE( "a native child popup does not republish an expired activity owner",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_surface_descriptor> descriptors;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    std::optional<semantic_surface_scope> activity;
    activity.emplace( manager, "activity_distraction", "Activity distraction",
                      std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> {
        { "activity.continue", "", "Continue", true }
    } );
    const std::string activity_surface = manager.top()->surface_id;

    // This mirrors the non-semantic branch of cancel_activity_or_ignore_query:
    // the native popup, not the parent activity surface, owns its input.  Its
    // retired parent stays private while the actionless native-return
    // successor supplies the deferred receipt.
    REQUIRE( manager.withhold_parent_authority_until_recreated( activity_surface ) );
    {
        semantic_surface_scope prompt( manager, "prompt", "Stop waiting?", {}, {
            { "prompt.choose", "prompt-option:1", "YES", true }
        }, []( const semantic_action_request & ) {
            return semantic_action_dispatch_result{ true };
        } );
        REQUIRE( manager.top() );
        CHECK( manager.top()->kind == "prompt" );
        const semantic_surface_descriptor prompt_frame = *manager.top();
        REQUIRE( manager.submit_request( { "test-run", prompt_frame.surface_id, prompt_frame.frame_id,
                                           "ignore", "prompt.choose", "prompt-option:1", {} } ) );
        REQUIRE( prompt.consume_request() );
    }

    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );
    std::string return_frame;
    {
        semantic_surface_scope activity_return( manager, "activity_resumed", "Activity resumed" );
        REQUIRE( manager.top() );
        return_frame = manager.top()->frame_id;
        CHECK( manager.top()->valid_actions.empty() );
    }

    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "ignore" );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().resulting_frame_id == return_frame );
    CHECK( std::none_of( descriptors.begin(), descriptors.end(),
    [&activity_surface]( const semantic_surface_descriptor & descriptor ) {
        return descriptor.surface_id == activity_surface &&
               descriptor.frame_id != "test-run:frame:1";
    } ) );
}

TEST_CASE( "activity pause receipts publish before the cancellation prompt unwinds",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    std::optional<semantic_surface_scope> activity;
    activity.emplace( manager, "activity_wait", "Activity in progress",
                      std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> {
        { "activity.pause", "", "Pause activity", true }
    }, []( const semantic_action_request & request ) {
        if( request.action_id != "activity.pause" ) {
            return semantic_action_dispatch_result{ false, "unadvertised_action", "" };
        }
        // DEFAULTMODE already owns the native pause action.  The cancellation
        // query is a separate prompt owner, so receipt this action immediately.
        return semantic_action_dispatch_result{ true, "", "", false, false };
    } );
    const semantic_surface_descriptor frame = *manager.top();
    REQUIRE( manager.submit_request( { "test-run", frame.surface_id, frame.frame_id,
                                       "pause-activity", "activity.pause", std::nullopt, {} } ) );
    REQUIRE( activity->consume_request() );

    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().action_id == "activity.pause" );
    CHECK( receipts.front().resulting_frame_id.empty() );

    REQUIRE( manager.withhold_parent_authority_until_recreated( frame.surface_id ) );
    {
        semantic_surface_scope prompt( manager, "prompt", "Stop waiting?", {}, {
            { "prompt.choose", "prompt-option:1", "YES", true }
        } );
        CHECK( manager.top()->kind == "prompt" );
        CHECK( receipts.size() == 1 );
    }
    CHECK_FALSE( manager.top() );
    CHECK( receipts.size() == 1 );
}

TEST_CASE( "dialogue semantic receipts are emitted before a native modal",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World" );

    {
        semantic_surface_scope dialogue( manager, "dialogue", "NPC", {}, {
            { "dialogue.choose", "response-1", "Continue", true }
        }, []( const semantic_action_request & ) {
            return semantic_action_dispatch_result{ true, "", "", true, false };
        } );
        const semantic_surface_descriptor dialogue_frame = *manager.top();
        REQUIRE( manager.submit_request( { "test-run", dialogue_frame.surface_id,
                                           dialogue_frame.frame_id, "choose", "dialogue.choose",
                                           "response-1", {} } ) );
        REQUIRE( dialogue.consume_request() );
        REQUIRE( receipts.size() == 1 );
        CHECK( receipts.front().request_id == "choose" );
        CHECK( receipts.front().accepted );
        CHECK( receipts.front().consuming_frame_id == dialogue_frame.frame_id );
        CHECK( receipts.front().resulting_frame_id.empty() );
    }

    REQUIRE( manager.top() );
    CHECK( manager.top()->kind == "world" );
    CHECK( receipts.size() == 1 );

    semantic_surface_scope successor( manager, "dialogue", "NPC", {}, {
        { "dialogue.cancel", "", "Goodbye", true }
    } );
    CHECK( receipts.size() == 1 );
    CHECK( manager.top()->kind == "dialogue" );
}

TEST_CASE( "terminal main menu intent is receipted without a GUI successor",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    bool native_quit_selected = false;
    semantic_surface_scope menu( manager, "main_menu", "Main menu", {}, {
        { "main_menu.quit", "", "Quit", true }
    }, [&native_quit_selected]( const semantic_action_request & request ) {
        if( request.action_id != "main_menu.quit" || request.stable_id ) {
            return semantic_action_dispatch_result{ false, "unadvertised_action", "" };
        }
        native_quit_selected = true;
        return semantic_action_dispatch_result{ true, "", "", false, false };
    } );
    const semantic_surface_descriptor frame = *manager.top();

    REQUIRE( manager.submit_request( { "test-run", frame.surface_id, frame.frame_id,
                                       "quit", "main_menu.quit", std::nullopt, {} } ) );
    REQUIRE( menu.consume_request() );
    REQUIRE( native_quit_selected );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "quit" );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().resulting_frame_id.empty() );

    REQUIRE( manager.submit_request( { "test-run", "other-surface", frame.frame_id,
                                       "wrong-owner", "main_menu.quit", std::nullopt, {} } ) );
    CHECK_FALSE( menu.consume_request() );
    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.back().rejection_reason == "wrong_surface" );

    REQUIRE( manager.submit_request( { "test-run", frame.surface_id, "stale-frame",
                                       "stale", "main_menu.quit", std::nullopt, {} } ) );
    CHECK_FALSE( menu.consume_request() );
    REQUIRE( receipts.size() == 3 );
    CHECK( receipts.back().rejection_reason == "stale_frame" );
}

TEST_CASE( "a repeated overmap destination request receipts without an unchanged successor",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "repeat-terminal-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    {
        semantic_surface_scope overmap( manager, "overmap", "Overmap", {}, {
            { "overmap.choose_destination", "omt:1,1,0", "Choose destination", true }
        }, []( const semantic_action_request & ) {
            // The native route selection may preserve this exact descriptor.
            // Its receipt therefore cannot depend on a future republish.
            return semantic_action_dispatch_result{ true, "", "", false, false };
        } );
        const semantic_surface_descriptor first = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), first.surface_id, first.frame_id,
                                           "first", "overmap.choose_destination", "omt:1,1,0", {} } ) );
        REQUIRE( overmap.consume_request() );
        REQUIRE( receipts.size() == 1 );
        CHECK( receipts.front().request_id == "first" );
        CHECK( receipts.front().resulting_frame_id.empty() );

        const semantic_surface_descriptor second = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), second.surface_id, second.frame_id,
                                           "second", "overmap.choose_destination", "omt:1,1,0", {} } ) );
        REQUIRE( overmap.consume_request() );
        CHECK( receipts.size() == 2 );
    }

    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.back().request_id == "second" );
    CHECK( receipts.back().accepted );
    CHECK( receipts.back().resulting_frame_id.empty() );
}

TEST_CASE( "a queued semantic request re-wakes its native owner after FIFO wake loss",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "queued-wake-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope overmap( manager, "overmap", "Overmap", {}, {
        { "overmap.choose_destination", "omt:1,1,0", "Choose destination", true }
    }, []( const semantic_action_request & ) {
        return semantic_action_dispatch_result{ true, "", "", false, false };
    } );
    const semantic_surface_descriptor frame = *manager.top();

    // This models the JSONL record already being queued after an inner
    // context consumed its FIFO byte.  No new transport bytes are available
    // on the next SDL poll, so pending work must still wake the true owner.
    REQUIRE( manager.submit_request( { manager.run_id(), frame.surface_id, frame.frame_id,
                                       "queued-after-wake-loss", "overmap.choose_destination",
                                       "omt:1,1,0", {} } ) );
    CHECK( poll_active_semantic_surface_request() );
    CHECK( take_active_semantic_surface_wake() );
    REQUIRE( overmap.consume_request() );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "queued-after-wake-loss" );
    CHECK( receipts.front().accepted );
}

TEST_CASE( "semantic transport records JSONL offsets through a lost FIFO wake",
           "[semantic_surface][semantic_transport_bytes_067]" )
{
    const std::string mode = GENERATE( "crlf_successive", "crlf_batch", "lf_successive" );
    CAPTURE( mode );
    const bool batch = mode == "crlf_batch";
    const std::string newline = mode == "lf_successive" ? "\n" : "\r\n";
    const std::size_t first_end = 477 + newline.size();
    const std::size_t final_end = first_end + 503 + newline.size();
    const std::filesystem::path transport_path = std::filesystem::temp_directory_path() /
            ( "semantic-surface-transport-" + std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count() ) + ".jsonl" );
    const char *const previous_path = std::getenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH" );
    const bool had_previous_path = previous_path != nullptr;
    const std::string saved_path = had_previous_path ? previous_path : "";
    on_out_of_scope cleanup( [&] {
        if( had_previous_path )
        {
            setenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH", saved_path.c_str(), 1 );
        } else
        {
            unsetenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH" );
        }
        std::filesystem::remove( transport_path );
    } );
    setenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH", transport_path.string().c_str(), 1 );

    semantic_surface_manager manager( "transport-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_request_transport_event> transport_events;
    std::vector<semantic_action_receipt> receipts;
    std::vector<std::string> dispatched;
    manager.set_transport_observer( [&transport_events]( const semantic_request_transport_event &
    event ) {
        transport_events.push_back( event );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope overmap( manager, "overmap", "Overmap", {}, {
        { "overmap.choose_destination", "omt:1,1,0", "Choose destination", true }
    }, [&dispatched]( const semantic_action_request & request ) {
        dispatched.push_back( request.request_id );
        return semantic_action_dispatch_result{ true, "", "", false, false };
    } );
    const semantic_surface_descriptor first = *manager.top();
    const auto append_record = [&]( const semantic_surface_descriptor & frame,
    const std::string & request_id, std::size_t body_size ) {
        std::string record = "{\"run_id\":\"transport-run\",\"surface_id\":\"" + frame.surface_id +
                             "\",\"frame_id\":\"" + frame.frame_id + "\",\"request_id\":\"" + request_id +
                             "\",\"action_id\":\"overmap.choose_destination\",\"stable_id\":\"omt:1,1,0\"}";
        REQUIRE( record.size() <= body_size );
        record.resize( body_size, ' ' );
        record += newline;
        std::ofstream stream( transport_path, std::ios::binary | std::ios::app );
        REQUIRE( stream );
        stream.write( record.data(), static_cast<std::streamsize>( record.size() ) );
        REQUIRE( stream );
    };
    append_record( first, "first", 477 );
    if( batch ) {
        append_record( first, "second", 503 );
    }
    REQUIRE( manager.poll_request_transport() );
    const auto first_record = std::find_if( transport_events.begin(), transport_events.end(),
    []( const semantic_request_transport_event & event ) {
        return event.event == "jsonl_record" && event.request_id == "first";
    } );
    REQUIRE( first_record != transport_events.end() );
    CHECK( first_record->queued );
    CHECK( first_record->offset_before == 0 );
    CHECK( first_record->offset_after == first_end );
    CHECK( first_record->transport_end == ( batch ? final_end : first_end ) );

    // A lost FIFO wake must not hide queued work.  This also polls the empty
    // tail: a CRLF text reader on Windows used to advance one byte here.
    REQUIRE( poll_active_semantic_surface_request() );
    REQUIRE( take_active_semantic_surface_wake() );
    REQUIRE( overmap.consume_request() );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "first" );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().requested_run_id == first.run_id );
    CHECK( receipts.front().requested_surface_id == first.surface_id );
    CHECK( receipts.front().requested_frame_id == first.frame_id );
    CHECK( std::any_of( transport_events.begin(), transport_events.end(),
    []( const semantic_request_transport_event & event ) {
        return event.event == "wake_marked";
    } ) );
    CHECK( std::any_of( transport_events.begin(), transport_events.end(),
    []( const semantic_request_transport_event & event ) {
        return event.event == "request_consumed" && event.request_id == "first";
    } ) );

    semantic_surface_descriptor second = first;
    if( !batch ) {
        CHECK_FALSE( manager.poll_request_transport() );
        REQUIRE( overmap.publish( { { "route_destination", "1,1,0" } }, first.valid_actions ) );
        second = *manager.top();
        append_record( second, "second", 503 );
        REQUIRE( manager.poll_request_transport() );
    }
    const auto second_record = std::find_if( transport_events.begin(), transport_events.end(),
    []( const semantic_request_transport_event & event ) {
        return event.event == "jsonl_record" && event.request_id == "second";
    } );
    REQUIRE( second_record != transport_events.end() );
    CHECK( second_record->queued );
    CHECK( second_record->offset_before == ( batch ? 0 : first_end ) );
    CHECK( second_record->offset_after == final_end );
    CHECK( second_record->transport_end == final_end );
    CHECK( std::filesystem::file_size( transport_path ) == final_end );
    REQUIRE( poll_active_semantic_surface_request() );
    REQUIRE( take_active_semantic_surface_wake() );
    REQUIRE( overmap.consume_request() );
    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.back().request_id == "second" );
    CHECK( receipts.back().accepted );
    CHECK( receipts.back().requested_run_id == second.run_id );
    CHECK( receipts.back().requested_surface_id == second.surface_id );
    CHECK( receipts.back().requested_frame_id == second.frame_id );
    CHECK( ( dispatched == std::vector<std::string> { "first", "second" } ) );
    CHECK_FALSE( manager.poll_request_transport() );
    CHECK_FALSE( poll_active_semantic_surface_request() );
    CHECK_FALSE( overmap.consume_request() );
    CHECK( receipts.size() == 2 );
    CHECK( dispatched.size() == 2 );
}

TEST_CASE( "item info is a semantic child and restores its selector parent",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope inventory( manager, "inventory", "Inventory", {}, {
        { "inventory.details", "rock-uid", "Details", true }
    }, []( const semantic_action_request & ) {
        return semantic_action_dispatch_result{ true, "", "", true };
    } );
    const semantic_surface_descriptor inventory_descriptor = *manager.top();
    REQUIRE( manager.submit_request( { "test-run", inventory_descriptor.surface_id,
                                       inventory_descriptor.frame_id, "details", "inventory.details",
                                       "rock-uid", {} } ) );
    REQUIRE( inventory.consume_request() );
    CHECK( receipts.empty() );

    item_info_data data( "rock", "rock", {
        iteminfo( "BASE", "Weight: ", "<num> kg", iteminfo::lower_is_better, 1 ),
        iteminfo( "DESCRIPTION", "A palm-sized stone." ),
        iteminfo( "DESCRIPTION", "Undisplayed description", iteminfo::no_name ),
        iteminfo( "ARMOR", "Protection;Bash\nTorso;2", iteminfo::is_table )
    }, {
        iteminfo( "BASE", "Weight: ", "<num> kg", iteminfo::lower_is_better, 2 ),
        iteminfo( "DESCRIPTION", "Comparison-only description" )
    } );
    std::string details_frame;
    manager.set_descriptor_observer( [&manager, &data, &details_frame](
    const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "item_info" ) {
            return;
        }
        REQUIRE( descriptor.breadcrumbs == std::vector<std::string> { "Inventory", "Item info" } );
        REQUIRE( descriptor.valid_actions.size() == 1 );
        CHECK( descriptor.valid_actions.front().id == "item_info.close" );
        REQUIRE( descriptor.payload.count( "item_info_text" ) == 1 );
        const std::string &text = descriptor.payload.at( "item_info_text" );
        CHECK( text == format_item_info( data.get_item_display(), data.get_item_compare() ) );
        CHECK( text.find( "Weight: " ) != std::string::npos );
        CHECK( text.find( "<color_c_light_green>1</color> kg" ) != std::string::npos );
        CHECK( text.find( "A palm-sized stone." ) != std::string::npos );
        CHECK( text.find( "Protection: Bash\n  Torso: 2" ) != std::string::npos );
        CHECK( text.find( "Undisplayed description" ) == std::string::npos );
        CHECK( text.find( "Comparison-only description" ) == std::string::npos );
        CHECK( descriptor.payload.at( "item_info_text_source" ) ==
               "format_item_info(item_info_data::get_item_display(), item_info_data::get_item_compare())" );
        REQUIRE( details_frame.empty() );
        details_frame = descriptor.frame_id;
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           "close-details", "item_info.close", std::nullopt, {} } ) );
    } );
    iteminfo_window details( data, point::zero, 80, 0 );
    details.execute();

    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.front().request_id == "details" );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().resulting_frame_id != inventory_descriptor.frame_id );
    CHECK( receipts.back().request_id == "close-details" );
    CHECK( receipts.back().accepted );
    // The redraw before consumption must retain the frame carrying these facts.
    CHECK( receipts.back().consuming_frame_id == details_frame );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == inventory.surface_id() );
    CHECK( manager.top()->frame_id != inventory_descriptor.frame_id );
}

TEST_CASE( "inventory selector resolves semantic item controls by live UID", "[semantic_surface]" )
{
    Character &avatar = get_player_character();
    clear_avatar();
    item_location selected = avatar.i_add( item( itype_id( "2x4" ) ), false );
    REQUIRE( selected );

    test_inventory_selector selector( avatar );
    selector.add_character_items( avatar );
    const std::string selected_id = std::to_string( selected->uid().get_value() );
    const auto actions = selector.semantic_actions( {
        { "inventory.commit", "", "Select", true },
        { "inventory.toggle", "", "Toggle selection", true }
    } );
    const auto select = std::find_if( actions.begin(), actions.end(), [&selected_id](
    const semantic_action_descriptor & action ) {
        return action.id == "inventory.select" && action.stable_id == selected_id;
    } );
    REQUIRE( select != actions.end() );
    CHECK( std::any_of( actions.begin(), actions.end(), [&selected_id](
    const semantic_action_descriptor & action ) {
        return action.id == "inventory.toggle" && action.stable_id == selected_id;
    } ) );

    std::optional<inventory_input> native_input;
    const semantic_action_dispatch_result selected_result = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "select", "inventory.select", selected_id, {}
    }, native_input );
    CHECK( selected_result.accepted );
    CHECK( selected_result.await_child_successor );
    REQUIRE( native_input );
    CHECK( native_input->action == "CONFIRM" );
    CHECK( native_input->semantic_target == selected );

    native_input.reset();
    const semantic_action_dispatch_result details_result = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "details", "inventory.details", selected_id, {}
    }, native_input );
    CHECK( details_result.accepted );
    CHECK( details_result.await_child_successor );
    REQUIRE( native_input );
    CHECK( native_input->action == "EXAMINE" );
    CHECK( native_input->semantic_target == selected );

    avatar.i_rem( &*selected );
    native_input.reset();
    const semantic_action_dispatch_result stale_result = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "stale", "inventory.select", selected_id, {}
    }, native_input );
    CHECK_FALSE( stale_result.accepted );
    CHECK( stale_result.rejection_reason == "invalid_stable_id" );
    CHECK_FALSE( native_input );
}

TEST_CASE( "inventory selector keeps UID controls bound across common actions",
           "[semantic_surface]" )
{
    Character &avatar = get_player_character();
    clear_avatar();
    item backpack( itype_id( "backpack" ) );
    item plank( itype_id( "2x4" ) );
    item rock( itype_id( "rock" ) );
    item another_rock( itype_id( "rock" ) );
    item bottle( itype_id( "bottle_plastic" ) );
    another_rock.set_var( "item_note", "second rock" );
    REQUIRE( backpack.put_in( plank, pocket_type::CONTAINER ).success() );
    REQUIRE( backpack.put_in( rock, pocket_type::CONTAINER ).success() );
    REQUIRE( backpack.put_in( another_rock, pocket_type::CONTAINER ).success() );
    REQUIRE( backpack.put_in( bottle, pocket_type::CONTAINER ).success() );
    avatar.wield( backpack );
    REQUIRE( avatar.get_wielded_item()->uid().is_valid() );
    const std::vector<item_location> live_items = avatar.all_items_loc();
    const auto live_rock = std::find_if( live_items.begin(),
    live_items.end(), []( const item_location & location ) {
        return location && location->typeId() == itype_id( "rock" );
    } );
    const auto second_rock = std::find_if( std::next( live_rock ),
    live_items.end(), []( const item_location & location ) {
        return location && location->typeId() == itype_id( "rock" );
    } );
    const auto live_bottle = std::find_if( live_items.begin(),
    live_items.end(), []( const item_location & location ) {
        return location && location->typeId() == itype_id( "bottle_plastic" );
    } );
    REQUIRE( live_rock != live_items.end() );
    REQUIRE( second_rock != live_items.end() );
    REQUIRE( live_bottle != live_items.end() );
    const std::string rock_id = std::to_string( ( *live_rock )->uid().get_value() );
    const std::string second_rock_id = std::to_string( ( *second_rock )->uid().get_value() );
    const std::string bottle_id = std::to_string( ( *live_bottle )->uid().get_value() );

    // These live entries have different native item types, so the selector
    // must not aggregate them before it advertises their UIDs.
    test_inventory_selector selector( avatar );
    selector.add_character_items( avatar );
    const auto actions = selector.semantic_actions( {
        { "inventory.toggle", "", "Toggle selection", true },
        { "inventory.wield", "", "Wield", true },
        { "inventory.wear", "", "Wear", true }
    } );
    const auto advertised_for = [&actions]( const std::string & action_id,
    const std::string & stable_id ) {
        return std::any_of( actions.begin(), actions.end(), [&action_id, &stable_id](
        const semantic_action_descriptor & action ) {
            return action.id == action_id && action.stable_id == stable_id && action.enabled;
        } );
    };
    CHECK( advertised_for( "inventory.select", rock_id ) );
    CHECK( advertised_for( "inventory.select", second_rock_id ) );
    CHECK( advertised_for( "inventory.details", rock_id ) );
    CHECK( advertised_for( "inventory.contents", bottle_id ) );
    CHECK( advertised_for( "inventory.toggle", rock_id ) );
    CHECK( advertised_for( "inventory.wield", rock_id ) );
    CHECK_FALSE( advertised_for( "inventory.wear", rock_id ) );

    std::optional<inventory_input> native_input;
    const auto details = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "details", "inventory.details", rock_id, {}
    }, native_input );
    CHECK( details.accepted );
    REQUIRE( native_input );
    CHECK( native_input->action == "EXAMINE" );

    native_input.reset();
    const auto contents = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "contents", "inventory.contents", bottle_id, {}
    }, native_input );
    CHECK( contents.accepted );
    REQUIRE( native_input );
    CHECK( native_input->action == "EXAMINE_CONTENTS" );

    const auto filtered = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "filter", "inventory.filter", std::nullopt,
        { { "text", "rock" } }
    }, native_input );
    CHECK( filtered.accepted );
    CHECK( selector.get_filter() == "rock" );
    const auto reset = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "reset", "inventory.reset_filter", std::nullopt, {}
    }, native_input );
    CHECK( reset.accepted );
    CHECK( selector.get_filter().empty() );

    native_input.reset();
    const auto cancel = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "cancel", "inventory.cancel", std::nullopt, {}
    }, native_input );
    CHECK( cancel.accepted );
    REQUIRE( native_input );
    CHECK( native_input->action == "QUIT" );

    native_input.reset();
    const auto second_rock_result = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "second-rock", "inventory.select", second_rock_id, {}
    }, native_input );
    CHECK( second_rock_result.accepted );
    REQUIRE( native_input );
    CHECK( native_input->semantic_target == *second_rock );

    const auto disabled_actions = selector.semantic_actions( {
        { "inventory.wield", "", "Wield", false }
    } );
    CHECK( std::any_of( disabled_actions.begin(), disabled_actions.end(), [&second_rock_id](
    const semantic_action_descriptor & action ) {
        return action.id == "inventory.wield" && action.stable_id == second_rock_id && !action.enabled;
    } ) );

    std::vector<item_location> item_locations = avatar.all_items_loc();
    auto rock_location = std::find_if( item_locations.begin(), item_locations.end(),
    [&rock_id]( const item_location & location ) {
        return location && std::to_string( location->uid().get_value() ) == rock_id;
    } );
    REQUIRE( rock_location != item_locations.end() );
    rock_location->remove_item();
    native_input.reset();
    const auto moved = selector.handle_semantic_request( {
        "test-run", "inventory", "frame", "moved", "inventory.select", rock_id, {}
    }, native_input );
    CHECK_FALSE( moved.accepted );
    CHECK( moved.rejection_reason == "invalid_stable_id" );
    CHECK_FALSE( native_input );
}

TEST_CASE( "unsupported semantic surface is actionless and rejects requests", "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    semantic_surface_scope world( manager, "world", "World", {}, {
        { "world.wait", "", "Wait", true }
    } );
    semantic_surface_scope unsupported( manager, "unsupported", "Legacy prompt", {
        { "stop_reason", "missing explicit semantic scope" }
    } );
    REQUIRE( manager.top() );
    CHECK( manager.top()->valid_actions.empty() );

    const semantic_action_receipt receipt = manager.reject_request( {
        "test-run", manager.top()->surface_id, manager.top()->frame_id,
        "request-1", "world.wait", std::nullopt, {}
    } );
    CHECK_FALSE( receipt.accepted );
    CHECK( receipt.rejection_reason == "no_native_binding" );
    CHECK( receipt.consuming_surface_id == unsupported.surface_id() );
}

TEST_CASE( "semantic surface manager sessions restore their enclosing owner",
           "[semantic_surface]" )
{
    semantic_surface_manager world_manager( "world-run" );
    semantic_surface_manager child_manager( "child-run" );
    CHECK( active_semantic_surface_manager() == nullptr );
    {
        semantic_surface_manager_session world_session( world_manager );
        CHECK( active_semantic_surface_manager() == &world_manager );
        {
            semantic_surface_manager_session child_session( child_manager );
            CHECK( active_semantic_surface_manager() == &child_manager );
        }
        CHECK( active_semantic_surface_manager() == &world_manager );
    }
    CHECK( active_semantic_surface_manager() == nullptr );
}

TEST_CASE( "semantic surface transport observes exact descriptors and rejections",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_surface_descriptor> descriptors;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    semantic_surface_scope world( manager, "world", "World", {}, {
        { "world.wait", "", "Wait", true }
    } );
    REQUIRE( descriptors.size() == 1 );
    CHECK( descriptors.back().surface_id == world.surface_id() );
    CHECK( descriptors.back().frame_id == manager.top()->frame_id );

    const semantic_action_receipt receipt = manager.reject_request( {
        "test-run", world.surface_id(), manager.top()->frame_id,
        "request-1", "world.wait", std::nullopt, {}
    } );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.back().request_id == receipt.request_id );
    CHECK( receipts.back().consuming_frame_id == receipt.consuming_frame_id );
}

TEST_CASE( "unchanged semantic surface publication preserves its frame", "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    semantic_surface_scope menu( manager, "menu", "Menu", { { "filter", "" } }, {
        { "menu.choose", "entry-a", "Entry A", true }
    } );
    const std::string initial_frame = manager.top()->frame_id;

    CHECK( menu.publish( { { "filter", "" } }, {
        { "menu.choose", "entry-a", "Entry A", true }
    } ) );
    CHECK( manager.top()->frame_id == initial_frame );

    CHECK( menu.publish( { { "filter", "changed" } }, {
        { "menu.choose", "entry-a", "Entry A", true }
    } ) );
    CHECK( manager.top()->frame_id != initial_frame );
}

TEST_CASE( "only the descriptor-bound top owner consumes a semantic request",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    int actions_consumed = 0;
    semantic_surface_scope world( manager, "world", "World", {}, {
        { "world.wait", "", "Wait", true }
    }, [&actions_consumed]( const semantic_action_request & request ) {
        ++actions_consumed;
        return semantic_action_dispatch_result{ true, "", request.frame_id };
    } );
    semantic_surface_descriptor descriptor = *manager.top();
    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "request-1", "world.wait", std::nullopt, {} } ) );
    CHECK( world.consume_request() );
    CHECK( actions_consumed == 1 );

    CHECK( world.publish( { { "state", "updated" } },
    { { "world.wait", "", "Wait", true } } ) );
    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "request-2", "world.wait", std::nullopt, {} } ) );
    CHECK_FALSE( world.consume_request() );
    CHECK( actions_consumed == 1 );

    const semantic_surface_descriptor fresh_descriptor = *manager.top();
    CHECK( manager.submit_request( { "test-run", fresh_descriptor.surface_id,
                                     fresh_descriptor.frame_id, "request-3", "world.move.n", std::nullopt,
                                     {} } ) );
    CHECK_FALSE( world.consume_request() );
    CHECK( actions_consumed == 1 );
}

TEST_CASE( "yielded parent frame fails closed while a native child owns input",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    semantic_surface_scope world( manager, "world", "World" );
    std::vector<semantic_action_receipt> receipts;
    std::vector<semantic_surface_descriptor> descriptors;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );

    std::optional<semantic_surface_scope> editor;
    const auto make_editor = [&manager, &editor]() {
        editor.emplace( manager, "map_editor", "Map editor", std::map<std::string, std::string> {},
        std::vector<semantic_action_descriptor> {
            { "editmap.confirm", "", "Confirm", true }
        } );
    };
    make_editor();
    const semantic_surface_descriptor retired_editor = *manager.top();

    // The editmap handoff must not briefly restore World.  That transient
    // descriptor can otherwise be mistaken for the post-selection owner.
    REQUIRE( manager.withhold_parent_authority_until_recreated( editor->surface_id() ) );
    editor.reset();
    CHECK_FALSE( manager.top() );
    {
        semantic_surface_scope field_menu( manager, "menu", "Select field", {}, {
            { "menu.choose", "fd_fire", "fire", true }
        } );
        const semantic_surface_descriptor field_descriptor = *manager.top();
        CHECK( field_descriptor.breadcrumbs == std::vector<std::string> { "World", "Select field" } );
        REQUIRE( manager.submit_request( { "test-run", retired_editor.surface_id,
                                           retired_editor.frame_id, "stale-editor", "editmap.confirm",
                                           std::nullopt, {} } ) );
        CHECK_FALSE( field_menu.consume_request() );
        REQUIRE( receipts.size() == 1 );
        CHECK_FALSE( receipts.back().accepted );
        // The retired editor descriptor names a different native owner, not
        // merely an earlier frame of the current child.  Keep that ownership
        // rejection exact while the parent remains withheld.
        CHECK( receipts.back().rejection_reason == "wrong_surface" );
        CHECK( receipts.back().consuming_surface_id == field_descriptor.surface_id );

        // Selecting the field immediately opens its intensity owner.  Keep
        // World private until the edit loop has recreated its scope.
        REQUIRE( manager.withhold_parent_authority_until_recreated( field_menu.surface_id() ) );
    }
    CHECK_FALSE( manager.top() );
    {
        semantic_surface_scope intensity_menu( manager, "menu", "Select intensity" );
        CHECK( manager.top()->breadcrumbs == std::vector<std::string> { "World", "Select intensity" } );
        REQUIRE( manager.withhold_parent_authority_until_recreated( intensity_menu.surface_id() ) );
    }
    CHECK_FALSE( manager.top() );

    // The edit loop recreates a fresh owner only after the nested menu exits.
    make_editor();
    REQUIRE( manager.top() );
    CHECK( manager.top()->kind == "map_editor" );
    CHECK( manager.top()->surface_id != retired_editor.surface_id );
    CHECK( std::none_of( descriptors.begin(), descriptors.end(),
    []( const semantic_surface_descriptor & descriptor ) {
        return descriptor.kind == "world" && descriptor.frame_id != "";
    } ) );
}

TEST_CASE( "semantic requests reject wrong identities and duplicate IDs without redispatch",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    int actions_consumed = 0;
    semantic_surface_scope world( manager, "world", "World", {}, {
        { "world.wait", "", "Wait", true }
    }, [&actions_consumed]( const semantic_action_request & ) {
        ++actions_consumed;
        return semantic_action_dispatch_result{ true, "", "" };
    } );
    semantic_surface_descriptor descriptor = *manager.top();
    CHECK( manager.submit_request( { "other-run", descriptor.surface_id, descriptor.frame_id,
                                     "wrong-run", "world.wait", std::nullopt, {} } ) );
    CHECK_FALSE( world.consume_request() );
    CHECK( manager.submit_request( { "test-run", "other-surface", descriptor.frame_id,
                                     "wrong-surface", "world.wait", std::nullopt, {} } ) );
    CHECK_FALSE( world.consume_request() );
    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "once", "world.wait", std::nullopt, {} } ) );
    CHECK( world.consume_request() );
    CHECK_FALSE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           "once", "world.wait", std::nullopt, {} } ) );
    CHECK( actions_consumed == 1 );
}

TEST_CASE( "accepted semantic receipts bind the native successor frame",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World", {}, {
        { "world.wait", "", "Wait", true }
    }, []( const semantic_action_request & ) {
        return semantic_action_dispatch_result{ true, "", "" };
    } );
    const semantic_surface_descriptor world_descriptor = *manager.top();

    CHECK( manager.submit_request( { "test-run", world_descriptor.surface_id,
                                     world_descriptor.frame_id, "request-1", "world.wait",
                                     std::nullopt, {} } ) );
    CHECK( world.consume_request() );
    CHECK( receipts.empty() );

    semantic_surface_scope wait_choice( manager, "menu", "Wait choice", {}, {
        { "wait.duration_menu", "", "Wait", true }
    } );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.back().accepted );
    CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
    CHECK( manager.top()->surface_id == wait_choice.surface_id() );
}

TEST_CASE( "zone manager semantic scope keeps a zone mutation bound to its displayed state",
           "[semantic_surface][zone_manager]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    const std::string zone_id = "zone-42";
    const std::string faction = "your_followers";
    int revision = 7;
    bool enabled = false;
    std::string native_action;
    const auto zone_consumer = [&zone_id, &faction, &revision, &enabled, &native_action]
    ( const semantic_action_request & request ) {
        if( request.stable_id.value_or( "" ) != zone_id ) {
            return semantic_action_dispatch_result{ false, "invalid_zone_id", "" };
        }
        if( request.parameters.at( "faction" ) != faction ) {
            return semantic_action_dispatch_result{ false, "wrong_faction", "" };
        }
        if( request.parameters.at( "revision" ) != std::to_string( revision ) ) {
            return semantic_action_dispatch_result{ false, "stale_revision", "" };
        }
        if( request.action_id != "zone.enable" || enabled ) {
            return semantic_action_dispatch_result{ false, "unavailable", "" };
        }
        native_action = "ENABLE_ZONE";
        return semantic_action_dispatch_result{ true, "", "" };
    };
    semantic_surface_scope zone_scope( manager, "zone_manager", "Zone Manager", {
        { "zone_id", zone_id }, { "faction", faction }, { "enabled", "false" }, { "revision", "7" }
    }, {
        { "zone.enable", zone_id, "Enable zone", true },
        { "zone.disable", zone_id, "Disable zone", false }
    }, zone_consumer );
    semantic_surface_descriptor descriptor = *manager.top();
    CHECK( descriptor.kind == "zone_manager" );
    CHECK( descriptor.payload.at( "zone_id" ) == zone_id );
    CHECK_FALSE( descriptor.valid_actions[1].enabled );

    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "wrong-owner", "zone.enable", zone_id,
    { { "faction", faction }, { "revision", "7" } } } ) );
    {
        semantic_surface_scope other_owner( manager, "menu", "Other" );
        CHECK_FALSE( other_owner.consume_request() );
    }
    REQUIRE_FALSE( receipts.empty() );
    CHECK( receipts.back().rejection_reason == "wrong_surface" );
    descriptor = *manager.top();

    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "bad-id", "zone.enable", "other-zone",
    { { "faction", faction }, { "revision", "7" } } } ) );
    CHECK_FALSE( zone_scope.consume_request() );
    CHECK( receipts.back().rejection_reason == "invalid_zone_id" );
    CHECK( native_action.empty() );
    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "wrong-faction", "zone.enable", zone_id,
    { { "faction", "other_faction" }, { "revision", "7" } } } ) );
    CHECK_FALSE( zone_scope.consume_request() );
    CHECK( receipts.back().rejection_reason == "wrong_faction" );
    CHECK( native_action.empty() );
    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "stale", "zone.enable", zone_id,
    { { "faction", faction }, { "revision", "6" } } } ) );
    CHECK_FALSE( zone_scope.consume_request() );
    CHECK( receipts.back().rejection_reason == "stale_revision" );
    CHECK( native_action.empty() );
    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "enable", "zone.enable", zone_id,
    { { "faction", faction }, { "revision", "7" } } } ) );
    CHECK( zone_scope.consume_request() );
    CHECK( native_action == "ENABLE_ZONE" );
    CHECK_FALSE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           "enable", "zone.enable", zone_id,
    { { "faction", faction }, { "revision", "7" } } } ) );
    enabled = true;
    ++revision;
    semantic_surface_scope successor( manager, "zone_manager", "Zone Manager", {
        { "zone_id", zone_id }, { "faction", faction }, { "enabled", "true" }, { "revision", "8" }
    }, { { "zone.disable", zone_id, "Disable zone", true } }, zone_consumer );
    REQUIRE_FALSE( receipts.empty() );
    CHECK( receipts.back().accepted );
    CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );

    const semantic_surface_descriptor successor_descriptor = *manager.top();
    CHECK( manager.submit_request( { "test-run", successor_descriptor.surface_id,
                                     successor_descriptor.frame_id, "already-enabled", "zone.enable", zone_id,
    { { "faction", faction }, { "revision", "8" } } } ) );
    CHECK_FALSE( successor.consume_request() );
    CHECK( receipts.back().rejection_reason == "unadvertised_action" );
    CHECK( native_action == "ENABLE_ZONE" );
}

TEST_CASE( "top-only request consumption emits an exact rejection without a native binding",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope unsupported( manager, "unsupported", "Legacy prompt", {
        { "stop_reason", "missing explicit semantic scope" }
    } );
    const semantic_surface_descriptor descriptor = *manager.top();

    CHECK( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                     "request-1", "world.wait", std::nullopt, {} } ) );
    CHECK_FALSE( unsupported.consume_request() );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "request-1" );
    CHECK( receipts.front().requested_surface_id == descriptor.surface_id );
    CHECK( receipts.front().requested_frame_id == descriptor.frame_id );
    CHECK( receipts.front().consuming_surface_id == descriptor.surface_id );
    CHECK( receipts.front().consuming_frame_id == descriptor.frame_id );
    CHECK_FALSE( receipts.front().accepted );
    CHECK( receipts.front().rejection_reason == "unadvertised_action" );
    CHECK_FALSE( unsupported.consume_request() );
    CHECK( receipts.size() == 1 );
}

TEST_CASE( "uilist consumes a stable semantic choice through its native query loop",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    semantic_surface_descriptor menu_descriptor;
    bool selection_requested = false;
    bool choice_requested = false;
    manager.set_descriptor_observer( [&manager, &menu_descriptor, &selection_requested,
                                                &choice_requested](
    const semantic_surface_descriptor & descriptor ) {
        menu_descriptor = descriptor;
        if( descriptor.kind != "menu" ) {
            return;
        }
        const auto action = std::find_if( descriptor.valid_actions.begin(),
        descriptor.valid_actions.end(), []( const semantic_action_descriptor & candidate ) {
            return candidate.label == "second";
        } );
        REQUIRE( action != descriptor.valid_actions.end() );
        if( !selection_requested ) {
            REQUIRE( action->id == "menu.select" );
            selection_requested = true;
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "select-second", action->id, action->stable_id, {} } ) );
        } else if( !choice_requested ) {
            const auto action = std::find_if( descriptor.valid_actions.begin(),
            descriptor.valid_actions.end(), []( const semantic_action_descriptor & candidate ) {
                return candidate.id == "menu.choose" && candidate.label == "second";
            } );
            REQUIRE( action != descriptor.valid_actions.end() );
            choice_requested = true;
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "choose-second", "menu.choose", action->stable_id, {} } ) );
        }
    } );

    uilist menu;
    menu.entries = {
        uilist_entry( 1, true, 'a', "first" ),
        uilist_entry( 2, true, 'b', "second" )
    };
    menu.query();

    CHECK( menu.ret == 2 );
    CHECK( menu.selected == 1 );
    CHECK( menu_descriptor.payload.at( "selected_stable_id" ) == menu.entries[1].semantic_stable_id );
}

TEST_CASE( "large searchable uilist requires a native semantic filter before choices",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    uilist menu;
    for( int index = 0; index < 3000; ++index ) {
        menu.entries.emplace_back( index, true, 0,
                                   "terrain-" + std::to_string( index ) + "-ordinary-entry-name" );
    }
    menu.entries[1777].txt = "opaque barrier terrain";
    const std::string stale_id = menu.entries[1777].semantic_stable_id;
    bool stale_requested = false;
    bool filter_requested = false;
    bool choice_requested = false;
    manager.set_descriptor_observer( [&manager, &stale_requested, &filter_requested, &choice_requested,
              stale_id]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "menu" ) {
            return;
        }
        const bool filter_required = descriptor.payload.at( "filter_required" ) == "true";
        if( filter_required && !stale_requested ) {
            stale_requested = true;
            REQUIRE( std::none_of( descriptor.valid_actions.begin(), descriptor.valid_actions.end(),
            []( const semantic_action_descriptor & action ) {
                return action.id == "menu.choose";
            } ) );
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "stale-pre-filter", "menu.choose", stale_id, {} } ) );
            filter_requested = true;
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "filter", "menu.filter", std::nullopt,
            { { "text", "opaque barrier" } } } ) );
        } else if( !filter_required && !choice_requested ) {
            const auto choice = std::find_if( descriptor.valid_actions.begin(), descriptor.valid_actions.end(),
            []( const semantic_action_descriptor & action ) {
                return action.id == "menu.choose" && action.label == "opaque barrier terrain";
            } );
            REQUIRE( choice != descriptor.valid_actions.end() );
            choice_requested = true;
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "choose-filtered", choice->id, choice->stable_id, {} } ) );
        }
    } );

    menu.query();
    REQUIRE( stale_requested );
    REQUIRE( filter_requested );
    REQUIRE( choice_requested );
    REQUIRE_FALSE( receipts.empty() );
    CHECK_FALSE( receipts.front().accepted );
    CHECK( receipts.front().rejection_reason == "unadvertised_action" );
    CHECK( menu.ret == 1777 );
}

TEST_CASE( "uilist can defer a selected receipt to its native child", "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World" );
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "menu" ) {
            return;
        }
        const auto action = std::find_if( descriptor.valid_actions.begin(),
        descriptor.valid_actions.end(), []( const semantic_action_descriptor & candidate ) {
            return candidate.id == "menu.choose";
        } );
        REQUIRE( action != descriptor.valid_actions.end() );
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           "choose-child", action->id, action->stable_id, {} } ) );
    } );

    uilist menu;
    menu.semantic_await_child_successor = true;
    menu.entries = { uilist_entry( 1, true, 'a', "open child" ) };
    menu.query();

    CHECK( menu.ret == 1 );
    CHECK( receipts.empty() );
    semantic_surface_scope child( manager, "dialogue", "NPC" );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().request_id == "choose-child" );
    CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
}

TEST_CASE( "child-owned receipt ignores a redraw of its selecting owner", "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World" );
    {
        semantic_surface_scope selector( manager, "menu", "Talk to whom?", {}, {
            { "menu.choose", "talker", "Talk to…", true }
        }, []( const semantic_action_request & ) {
            return semantic_action_dispatch_result{ true, "", "", true };
        } );
        const semantic_surface_descriptor selector_frame = *manager.top();
        REQUIRE( manager.submit_request( { "test-run", selector_frame.surface_id,
                                           selector_frame.frame_id, "choose-talker", "menu.choose",
                                           "talker", {} } ) );
        REQUIRE( selector.consume_request() );
        REQUIRE( manager.publish( selector.surface_id(), { { "selected", "talker" } },
        selector_frame.valid_actions ) );
        CHECK( receipts.empty() );
    }

    semantic_surface_scope dialogue( manager, "dialogue", "NPC" );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "choose-talker" );
    CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
}

TEST_CASE( "a deferred World chat receipt resolves only at its child owner",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    std::vector<semantic_surface_descriptor> descriptors;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );

    std::optional<semantic_surface_scope> world;
    world.emplace( manager, "world", "World", std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> {
        { "world.chat", "", "Chat", true }
    }, []( const semantic_action_request & ) {
        return semantic_action_dispatch_result{ true, "", "", true };
    } );
    const semantic_surface_descriptor world_frame = *manager.top();
    REQUIRE( manager.submit_request( { "test-run", world_frame.surface_id, world_frame.frame_id,
                                       "chat-child", "world.chat", std::nullopt, {} } ) );
    REQUIRE( world->consume_request() );
    world.reset();

    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );
    semantic_surface_scope chat_menu( manager, "menu", "What do you want to do?" );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
    CHECK( std::none_of( descriptors.begin(), descriptors.end(),
    [&world_frame]( const semantic_surface_descriptor & descriptor ) {
        return descriptor.surface_id == world_frame.surface_id &&
               descriptor.frame_id != world_frame.frame_id;
    } ) );
}

TEST_CASE( "a sentence prompt receipt survives its nested chat unwind", "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    std::optional<semantic_surface_scope> world;
    world.emplace( manager, "world", "World" );
    std::optional<semantic_surface_scope> prompt;
    prompt.emplace( manager, "string_prompt", "Text input", std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> { { "prompt.submit", "", "Submit", true } },
    []( const semantic_action_request & ) {
        return semantic_action_dispatch_result{ true, "", "", true };
    } );
    const semantic_surface_descriptor prompt_frame = *manager.top();
    REQUIRE( manager.submit_request( { "test-run", prompt_frame.surface_id, prompt_frame.frame_id,
                                       "submit-sentence", "prompt.submit", std::nullopt, {} } ) );
    REQUIRE( prompt->consume_request() );
    prompt.reset();
    world.reset();

    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );
    semantic_surface_scope next_world( manager, "world", "World" );
    REQUIRE( manager.top() );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
}

TEST_CASE( "native intent handoff is single-use and run-bound", "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    const semantic_action_request request{ "test-run", "main-menu", "frame", "quit-request",
                                           "main_menu.quit", std::nullopt, {} };
    CHECK( manager.queue_native_intent( request, "main_menu.quit" ) );
    CHECK_FALSE( manager.queue_native_intent( request, "main_menu.quit" ) );
    CHECK_FALSE( manager.take_native_intent( "wrong-intent" ) );
    CHECK( manager.take_native_intent( "main_menu.quit" ) );
    CHECK_FALSE( manager.take_native_intent( "main_menu.quit" ) );
    CHECK_FALSE( manager.queue_native_intent( { "wrong-run", "main-menu", "frame", "stale-request",
                 "main_menu.quit", std::nullopt, {} }, "main_menu.quit" ) );
}

TEST_CASE( "item-use restoration publishes only its recreated menu owner", "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_surface_descriptor> descriptors;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World" );
    {
        semantic_surface_scope item_menu( manager, "inventory_item_menu", "lighter", {}, {
            { "inventory.item_menu.choose", "activate", "activate", true }
        }, []( const semantic_action_request & ) {
            return semantic_action_dispatch_result{ true, "", "" };
        } );
        const semantic_surface_descriptor item_menu_frame = *manager.top();
        REQUIRE( manager.submit_request( { "test-run", item_menu_frame.surface_id,
                                           item_menu_frame.frame_id, "activate-lighter",
                                           "inventory.item_menu.choose", "activate", {} } ) );
        REQUIRE( item_menu.consume_request() );
        REQUIRE( manager.withhold_parent_authority_until_recreated( item_menu.surface_id() ) );
        {
            semantic_surface_scope direction( manager, "direction", "Light where?", {}, {
                { "direction.choose", "north", "North", true }
            }, []( const semantic_action_request & ) {
                return semantic_action_dispatch_result{ true, "", "" };
            } );
            const semantic_surface_descriptor direction_frame = *manager.top();
            REQUIRE( manager.submit_request( { "test-run", direction_frame.surface_id,
                                               direction_frame.frame_id, "choose-north",
                                               "direction.choose", "north", {} } ) );
            REQUIRE( direction.consume_request() );
        }
        CHECK_FALSE( manager.top() );
    }

    // The outer native loop may publish World while the old menu is between
    // destruction and recreation.  That publication stays private.
    REQUIRE( manager.publish( world.surface_id(), {}, {} ) );

    semantic_surface_scope recreated_item_menu( manager, "inventory_item_menu", "lighter" );
    const std::string recreated_frame = manager.top()->frame_id;
    REQUIRE( !descriptors.empty() );
    CHECK( descriptors.back().surface_id == recreated_item_menu.surface_id() );
    CHECK( descriptors.back().kind == "inventory_item_menu" );
    CHECK( std::none_of( descriptors.begin() + 2,
    descriptors.end(), []( const semantic_surface_descriptor & d ) {
        return d.kind == "world";
    } ) );
    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.back().request_id == "choose-north" );
    CHECK( receipts.back().resulting_frame_id == recreated_frame );
}

TEST_CASE( "direction selection receipts bind to its real confirmation successor",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_surface_descriptor> descriptors;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World" );
    {
        semantic_surface_scope item_menu( manager, "inventory_item_menu", "lighter" );
        REQUIRE( manager.withhold_parent_authority_until_recreated( item_menu.surface_id() ) );
        {
            semantic_surface_scope direction( manager, "direction", "Light where?", {}, {
                { "direction.choose", "north", "North", true }
            }, []( const semantic_action_request & ) {
                return semantic_action_dispatch_result{ true, "", "", true };
            } );
            const semantic_surface_descriptor direction_frame = *manager.top();
            REQUIRE( manager.submit_request( { "test-run", direction_frame.surface_id,
                                               direction_frame.frame_id, "choose-north",
                                               "direction.choose", "north", {} } ) );
            REQUIRE( direction.consume_request() );
        }
        CHECK( receipts.empty() );
        semantic_surface_scope prompt( manager, "prompt", "Light firewood?", {}, {
            { "prompt.choose", "no", "No", true }
        } );
        REQUIRE( receipts.size() == 1 );
        CHECK( receipts.front().request_id == "choose-north" );
        CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
    }
    CHECK( std::none_of( descriptors.begin() + 2,
    descriptors.end(), []( const semantic_surface_descriptor & d ) {
        return d.kind == "world" || d.kind == "inventory_item_menu";
    } ) );
}

TEST_CASE( "withheld main-menu quit releases its owner before publishing confirmation",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_surface_descriptor> descriptors;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    std::optional<semantic_surface_scope> main_menu;
    main_menu.emplace( manager, "main_menu", "Main menu", std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> { { "main_menu.quit", "", "Quit", true } },
    [&manager]( const semantic_action_request & request ) {
        if( !manager.withhold_parent_authority_until_recreated( request.surface_id ) ) {
            return semantic_action_dispatch_result{ false, "native_parent_withhold_failed", "" };
        }
        return semantic_action_dispatch_result{ true, "", "", true };
    } );
    const semantic_surface_descriptor main_menu_frame = *manager.top();
    REQUIRE( manager.submit_request( { "test-run", main_menu_frame.surface_id,
                                       main_menu_frame.frame_id, "quit", "main_menu.quit", "", {} } ) );
    REQUIRE( main_menu->consume_request() );

    // This matches the current-iteration quit path: no stale main-menu
    // descriptor may stand between the accepted action and query_yn's prompt.
    main_menu.reset();
    semantic_surface_scope confirmation( manager, "prompt", "YESNO", {
        { "text", "Really quit?" }
    }, { { "prompt.choose", "yes", "YES", true } } );

    REQUIRE( manager.top() );
    CHECK( manager.top()->kind == "prompt" );
    CHECK( manager.top()->breadcrumbs == std::vector<std::string> { "YESNO" } );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().request_id == "quit" );
    CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
    CHECK( std::none_of( descriptors.begin() + 1, descriptors.end(),
    []( const semantic_surface_descriptor & descriptor ) {
        return descriptor.kind == "main_menu";
    } ) );
}

TEST_CASE( "uilist rejects disabled and duplicate stable semantic choices",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );

    uilist disabled;
    disabled.entries = { uilist_entry( 1, false, 'd', "disabled" ) };
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind == "menu" ) {
            const semantic_action_descriptor &action = descriptor.valid_actions.front();
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "disabled-choice", action.id, action.stable_id, {} } ) );
        }
    } );
    disabled.query( false );
    REQUIRE( receipts.size() == 1 );
    CHECK_FALSE( receipts.back().accepted );
    CHECK( disabled.ret == UILIST_WAIT_INPUT );

    receipts.clear();
    uilist duplicate;
    duplicate.entries = {
        uilist_entry( 1, true, 'a', "first" ),
        uilist_entry( 2, true, 'b', "second" )
    };
    duplicate.entries[1].semantic_stable_id = duplicate.entries[0].semantic_stable_id;
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind == "menu" ) {
            const semantic_action_descriptor &action = descriptor.valid_actions.front();
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "duplicate-choice", action.id, action.stable_id, {} } ) );
        }
    } );
    duplicate.query( false );
    REQUIRE( receipts.size() == 1 );
    CHECK_FALSE( receipts.back().accepted );
    CHECK( receipts.back().rejection_reason == "duplicate_stable_id" );
}

TEST_CASE( "uilist semantic identity survives caller presentation reorder",
           "[semantic_surface]" )
{
    uilist_entry first( 1, true, 'a', "duplicate label" );
    uilist_entry second( 2, true, 'b', "duplicate label" );
    const std::string first_id = first.semantic_stable_id;
    const std::string second_id = second.semantic_stable_id;

    std::vector<uilist_entry> entries = { second, first };
    CHECK( entries[0].semantic_stable_id == second_id );
    CHECK( entries[1].semantic_stable_id == first_id );
    CHECK( entries[0].semantic_stable_id != entries[1].semantic_stable_id );
}

TEST_CASE( "ordinary debug menu chooses a child action by semantic stable ID",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "menu" ) {
            return;
        }
        if( descriptor.payload.at( "text" ) == "Debug Functions" ) {
            REQUIRE_FALSE( descriptor.breadcrumbs.empty() );
            CHECK( descriptor.breadcrumbs.back() == "Debug Functions" );
        }
        const std::string target = descriptor.payload.at( "text" ) == "Debug Functions" ? "Game…" :
                                   "Show debug message";
        const auto action = std::find_if( descriptor.valid_actions.begin(),
        descriptor.valid_actions.end(), [&target]( const semantic_action_descriptor & candidate ) {
            return candidate.id == "menu.choose" && candidate.label == target;
        } );
        REQUIRE( action != descriptor.valid_actions.end() );
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           descriptor.payload.at( "text" ), "menu.choose",
                                           action->stable_id, {} } ) );
    } );

    const std::optional<debug_menu::debug_menu_index> action = debug_menu::choose_action();
    REQUIRE( action );
    CHECK( *action == debug_menu::debug_menu_index::SHOW_MSG );
}

TEST_CASE( "debug map submenu publishes a semantic successor for Map editor",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    bool saw_map_menu = false;
    manager.set_descriptor_observer( [&manager,
    &saw_map_menu]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "menu" ) {
            return;
        }
        const std::string &text = descriptor.payload.at( "text" );
        const std::string target = text == "Debug Functions" ? "Map…" : "Map editor";
        const auto action = std::find_if( descriptor.valid_actions.begin(), descriptor.valid_actions.end(),
        [&target]( const semantic_action_descriptor & candidate ) {
            return candidate.id == "menu.choose" && candidate.label == target;
        } );
        REQUIRE( action != descriptor.valid_actions.end() );
        if( text == "Map…" ) {
            saw_map_menu = true;
            CHECK( descriptor.breadcrumbs.back() == "Map…" );
        }
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           text, "menu.choose", action->stable_id, {} } ) );
    } );

    const std::optional<debug_menu::debug_menu_index> action = debug_menu::choose_action();
    REQUIRE( action );
    CHECK( saw_map_menu );
    CHECK( *action == debug_menu::debug_menu_index::MAP_EDITOR );
}

TEST_CASE( "query popup consumes its bound semantic option in test mode",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind == "prompt" ) {
            const semantic_action_descriptor &action = descriptor.valid_actions.front();
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "accept-popup", "prompt.choose", action.stable_id, {} } ) );
        }
    } );

    const query_popup::result result = query_popup().message( "%s",
                                       "Continue?" ).option( "YES" ).query();
    CHECK( result.action == "YES" );
}

TEST_CASE( "auto-move cancellation popup binds YES to its native successor frame",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_manager_session session( manager );
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "prompt" ) {
            return;
        }
        const semantic_action_descriptor &yes = descriptor.valid_actions.front();
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           "cancel-auto-move", "prompt.choose", yes.stable_id, {} } ) );
    } );

    const query_popup::result result = query_popup()
                                       .context( "YESNO" )
                                       .message( "%s", "feral human spotted! Cancel auto move?" )
                                       .option( "YES" )
                                       .option( "NO" )
                                       .await_semantic_successor()
                                       .query();
    CHECK( result.action == "YES" );
    {
        semantic_surface_scope successor( manager, "auto_move_canceled", "Auto move canceled" );
        REQUIRE( receipts.size() == 1 );
        CHECK( receipts.front().request_id == "cancel-auto-move" );
        CHECK( receipts.front().accepted );
        CHECK_FALSE( receipts.front().resulting_frame_id.empty() );
        CHECK( receipts.front().resulting_frame_id == manager.top()->frame_id );
    }
}

TEST_CASE( "string prompt accepts constraint-valid structured semantic text",
           "[semantic_surface]" )
{
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    manager.set_descriptor_observer( [&manager]( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind == "string_prompt" ) {
            REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                               "submit-text", "prompt.submit", std::nullopt,
            { { "text", "42" } } } ) );
        }
    } );

    const catacurses::window window = catacurses::newwin( 3, 30, point::zero );
    REQUIRE( window );
    string_input_popup prompt;
    prompt.window( window, point( 1, 1 ),
                   28 ).title( "Number" ).text( "" ).max_length( 2 ).only_digits( true );
    CHECK( prompt.query_string() == "42" );
    CHECK( prompt.confirmed() );
    CHECK_FALSE( prompt.canceled() );
}

TEST_CASE( "imgui string prompt owns semantic coordinate submission and rejects stale requests",
           "[semantic_surface]" )
{
    scoped_imgui_context imgui;
    semantic_surface_manager manager( "test-run" );
    semantic_surface_manager_session session( manager );
    bool saw_prompt = false;
    bool rejected_wrong_surface = false;
    bool rejected_stale_frame = false;
    std::vector<std::string> rejection_reasons;
    manager.set_receipt_observer( [&rejection_reasons]( const semantic_action_receipt & receipt ) {
        if( !receipt.accepted ) {
            rejection_reasons.push_back( receipt.rejection_reason );
        }
    } );
    manager.set_descriptor_observer( [&manager, &saw_prompt, &rejected_wrong_surface,
                                                &rejected_stale_frame]
    ( const semantic_surface_descriptor & descriptor ) {
        if( descriptor.kind != "string_prompt" ) {
            return;
        }
        saw_prompt = true;
        CHECK( descriptor.valid_actions[0].id == "prompt.submit" );
        REQUIRE( manager.submit_request( { "test-run", "wrong-surface", descriptor.frame_id,
                                           "wrong", "prompt.submit", std::nullopt,
        { { "text", "0'152,0'49,0" } } } ) );
        rejected_wrong_surface = !manager.consume_top_request();
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, "stale-frame",
                                           "stale", "prompt.submit", std::nullopt,
        { { "text", "0'152,0'49,0" } } } ) );
        rejected_stale_frame = !manager.consume_top_request();
        REQUIRE( manager.submit_request( { "test-run", descriptor.surface_id, descriptor.frame_id,
                                           "submit", "prompt.submit", std::nullopt,
        { { "text", "0'152,0'49,0" } } } ) );
    } );

    string_input_popup_imgui popup( 65, "", "Teleport where?" );
    CHECK( popup.query() == "0'152,0'49,0" );
    CHECK( saw_prompt );
    CHECK( rejected_wrong_surface );
    CHECK( rejected_stale_frame );
    CHECK( rejection_reasons == std::vector<std::string> { "wrong_surface", "stale_frame" } );
}

TEST_CASE( "rejected transport stays inside the native input owner until a real successor",
           "[semantic_surface][shared_input_owner_067]" )
{
    const bool same_poll = GENERATE( false, true );
    const std::string rejection = GENERATE( "wrong_run", "wrong_surface", "stale_frame", "duplicate" );
    const auto path = std::filesystem::temp_directory_path() /
                      ( "shared-input-owner-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count() ) + ".jsonl" );
    const char *old_path = std::getenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH" );
    const std::optional<std::string> previous = old_path ? std::optional<std::string>( old_path ) :
            std::nullopt;
    on_out_of_scope cleanup( [&]() {
        if( previous ) {
            setenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH", previous->c_str(), 1 );
        } else {
            unsetenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH" );
        }
        std::filesystem::remove( path );
    } );
    setenv( "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH", path.string().c_str(), 1 );
#if defined(TILES)
    software_render_fixture renderer;
    REQUIRE( renderer.available() );
    renderer_recovery_test_support::setup_software_ui();
#else
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
#endif
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    semantic_surface_manager manager( "shared-input-owner" );
    semantic_surface_manager_session session( manager );
    semantic_surface_scope world( manager, "world", "World" );
    int native_dispatches = 0;
    std::vector<semantic_surface_descriptor> publications;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & descriptor ) {
        publications.push_back( descriptor );
    } );
    semantic_surface_scope parent( manager, "inventory", "Inventory", {},
    { { "inventory.select", "item:1", "Select", true } },
    [&]( const semantic_action_request & ) {
        ++native_dispatches;
        return semantic_action_dispatch_result{ true, "", "", true, true };
    } );
    const auto original = *manager.top();
    const auto append = [&]( const std::string & run, const std::string & surface,
    const std::string & frame, const std::string & request ) {
        std::ofstream stream( path, std::ios::app );
        stream << "{\"run_id\":\"" << run << "\",\"surface_id\":\"" << surface
               << "\",\"frame_id\":\"" << frame << "\",\"request_id\":\"" << request
               << "\",\"action_id\":\"inventory.select\",\"stable_id\":\"item:1\"}\n";
    };
    if( rejection == "duplicate" ) {
        REQUIRE( manager.submit_request( { "foreign-run", original.surface_id, original.frame_id,
                                           "inert", "inventory.select", "item:1", {} } ) );
        REQUIRE_FALSE( manager.consume_top_request() );
    }
    manager.set_receipt_observer( [&]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
        if( receipt.request_id == "inert" ) {
            CHECK_FALSE( receipt.accepted );
            CHECK( receipt.rejection_reason == ( rejection == "duplicate" ? "wrong_run" : rejection ) );
            REQUIRE( manager.top() );
            CHECK( manager.top()->surface_id == original.surface_id );
            CHECK( manager.top()->frame_id == original.frame_id );
            if( !same_poll ) {
                append( manager.run_id(), original.surface_id, original.frame_id, "select" );
            }
        }
    } );
    append( rejection == "wrong_run" || rejection == "duplicate" ? "foreign-run" : manager.run_id(),
            rejection == "wrong_surface" ? "foreign-surface" : original.surface_id,
            rejection == "stale_frame" ? "foreign-frame" : original.frame_id, "inert" );
    if( same_poll ) {
        append( manager.run_id(), original.surface_id, original.frame_id, "select" );
    }
    input_context input( "INVENTORY" );
    input.register_action( "QUIT" );
    input.handle_input( 100 );
    CHECK( native_dispatches == 1 );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == original.surface_id );
    CHECK( manager.top()->frame_id == original.frame_id );
    if( native_dispatches != 1 ) {
        return;
    }
    REQUIRE( receipts.size() == 1 );
    {
        bool closed = false;
        semantic_surface_scope child( manager, "prompt", "Confirmation", {},
        { { "prompt.acknowledge", "", "Acknowledge", true } },
        [&]( const semantic_action_request & ) {
            closed = true;
            return semantic_action_dispatch_result{ true, "", "" };
        } );
        REQUIRE( receipts.size() == 2 );
        CHECK( receipts.back().accepted );
        CHECK( receipts.back().request_id == "select" );
        CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
        const auto child_frame = *manager.top();
        REQUIRE( manager.submit_request( { manager.run_id(), child_frame.surface_id,
                                           child_frame.frame_id, "ack", "prompt.acknowledge", std::nullopt, {} } ) );
        manager.mark_transport_wake();
        input.handle_input( 100 );
        CHECK( closed );
    }
    REQUIRE( receipts.size() == 3 );
    CHECK( receipts.back().request_id == "ack" );
    CHECK( receipts.back().accepted );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == original.surface_id );
    CHECK( manager.top()->frame_id != original.frame_id );
    REQUIRE( publications.size() >= 3 );
    CHECK( publications.back().frame_id == manager.top()->frame_id );
    REQUIRE( manager.submit_request( { manager.run_id(), original.surface_id, original.frame_id,
                                       "old-parent", "inventory.select", "item:1", {} } ) );
    CHECK_FALSE( parent.consume_request() );
    CHECK( receipts.back().rejection_reason == "stale_frame" );
    CHECK( native_dispatches == 1 );
}

#if !defined(TILES)
extern "C" int ungetch( int );

TEST_CASE( "debug prompt retains native keyboard continuation outside semantic mode",
           "[semantic_surface][debug_error_prompt_067]" )
{
    REQUIRE( active_semantic_surface_manager() == nullptr );
    const char *old_run = std::getenv( "OPENCLAW_HARNESS_SEMANTIC_RUN_ID" );
    const std::optional<std::string> previous = old_run ? std::optional<std::string>( old_run ) :
            std::nullopt;
    unsetenv( "OPENCLAW_HARNESS_SEMANTIC_RUN_ID" );
    on_out_of_scope restore_environment( [&]() {
        if( previous ) {
            setenv( "OPENCLAW_HARNESS_SEMANTIC_RUN_ID", previous->c_str(), 1 );
        } else {
            unsetenv( "OPENCLAW_HARNESS_SEMANTIC_RUN_ID" );
        }
    } );
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    }
    on_out_of_scope close_renderer( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
    restore_on_out_of_scope<bool> restore_mode( test_mode );
    test_mode = false;
    {
        restore_on_out_of_scope<catacurses::window> restore_screen( catacurses::stdscr );
        catacurses::stdscr = catacurses::window();
        CHECK( capture_debugmsg_during( [&]() {
            realDebugmsg( "off-mode-control.cpp", "1", "keyboard_control", "native keyboard error" );
        } ) == "native keyboard error" );
    }
    REQUIRE_FALSE( openclaw_harness_semantic_session_active() );
    // Queue at the raw debug prompt's intended read, after renderer setup;
    // no inventory editor/setup loop consumes or resets this native input.
    REQUIRE( ungetch( ' ' ) != -1 );
    REQUIRE_NOTHROW( replay_buffered_debugmsg_prompts() );
    CHECK( active_semantic_surface_manager() == nullptr );
}

TEST_CASE( "shared input ownership preserves bounded ordinary keyboard reads",
           "[semantic_surface][shared_input_owner_keyboard_067]" )
{
    const bool bound = GENERATE( false, true );
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    } else {
        catacurses::refresh();
    }
    on_out_of_scope close_owned_terminal( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    semantic_surface_manager manager( "physical-keyboard" );
    std::optional<semantic_surface_manager_session> session;
    std::optional<semantic_surface_scope> owner;
    if( bound ) {
        session.emplace( manager );
        owner.emplace( manager, "inventory", "Native inventory" );
    }
    input_context input( "INVENTORY" );
    input.register_action( "ANY_INPUT" );
    REQUIRE( ungetch( '1' ) != -1 );
    input.handle_input( 100 );
    CHECK( input.get_raw_input().type == input_event_t::keyboard_char );
    CHECK( input.get_raw_input().get_first_input() == '1' );
    if( bound ) {
        CHECK( manager.top()->surface_id == owner->surface_id() );
    } else {
        CHECK( active_semantic_surface_manager() == nullptr );
    }
}
#endif

TEST_CASE( "native journal bounds diagnostics while preserving current authority",
           "[semantic_surface][native_log_window]" )
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() /
                               ( "caol-native-window-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( directory );
    const fs::path trace = directory / "semantic.native.events.jsonl";
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_TRACE_PATH", "OPENCLAW_HARNESS_RAID_ACTOR_TRACE"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const std::string &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    on_out_of_scope cleanup( [&] {
        for( const auto &entry : previous )
        {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
        fs::remove_all( directory );
    } );
    const std::string run = directory.filename().string();
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    setenv( keys[2].c_str(), trace.string().c_str(), 1 );
    setenv( keys[3].c_str(), run.c_str(), 1 );
    raid_decision_trace::recorder diagnostic_recorder( { run, run, run, trace.string() } );
    semantic_surface_manager &manager = openclaw_harness_semantic_surface_manager();
    int dispatched = 0;
    std::vector<semantic_action_descriptor> actions;
    for( int index = 0; index < 200; ++index ) {
        actions.push_back( { "inventory.pick", "item:" + std::to_string( index ),
                             std::string( 4096, 'x' ), true } );
    }
    const std::string surface = manager.push( "inventory", "Inventory", {}, actions,
    [&]( const semantic_action_request & ) {
        ++dispatched;
        return semantic_action_dispatch_result{ true, "", "", false, true, "picked" };
    } );
    on_out_of_scope pop( [&] { manager.pop( surface ); } );
    const semantic_surface_descriptor issued = *manager.top();
    const semantic_action_request request = { run, issued.surface_id, issued.frame_id,
                                              "pick-once", "inventory.pick", "item:199", {}
                                            };
    REQUIRE( manager.submit_request( request ) );
    REQUIRE( manager.consume_top_request() );
    CHECK( dispatched == 1 );
    REQUIRE( manager.publish( surface, { { "selection", "item:199" } }, actions ) );
    // An action catalogue is lossless, including a non-front physical item.
    const std::string diagnostic( 96 * 1024, 'd' );
    for( const std::string state : {
             "active", "hostile_boundary", "resumed", "progress", "completed_cleared"
         } ) {
        REQUIRE( write_native_semantic_event( "{\"event\":\"travel\",\"run_id\":\"" + run +
                                              "\",\"travel_id\":\"trip-1\",\"receipt_id\":\"trip-1:" + state +
                                              "\",\"state\":\"" + state + "\",\"destination\":[129,149,0]," +
                                              "\"destination_present\":" + ( state == "completed_cleared" ? "false" : "true" ) +
                                              ",\"destination_cleared\":" + ( state == "completed_cleared" ? "true" : "false" ) + "}" ) );
    }
    std::set<std::string> travel_states;
    for( int index = 0; index < 100; ++index ) {
        std::ostringstream event;
        event << "{\"event\":\"turn\",\"run_id\":\"" << run
              << "\",\"diagnostic\":\"" << diagnostic << "\"}";
        REQUIRE( write_native_diagnostic_event( event.str() ) );
        diagnostic_recorder.record( "actor", "raid_actor_action", index,
                                    "\"action\":\"Pause\",\"npc_id\":5",
                                    std::to_string( index ) );
    }
    // These all use the same real recorder, including names without raid_.
    // A diagnostic resembling authority must not replace the current owner.
    const std::set<std::string> diagnostic_kinds = {
        "bandit_shakedown_emission", "bandit_shakedown_delivery", "bandit_shakedown_contact",
        "bandit_shakedown_fight_reply", "npc_alarm", "npc_attack_callback",
        "scout_perceived_threat", "scout_alarm_response", "surface_descriptor"
    };
    for( const std::string &kind : diagnostic_kinds ) {
        diagnostic_recorder.record_edge( kind, 100, "\"frame_id\":\"diagnostic-not-input\"" );
    }
    std::set<std::string> retained_diagnostic_kinds;
    std::ifstream input( trace );
    std::string line;
    std::string accepted_frame;
    bool accepted = false;
    bool current = false;
    bool raid_diagnostic = false;
    std::size_t discarded = 0;
    while( std::getline( input, line ) ) {
        const std::string prefix = "openclaw_harness_semantic_step: ";
        const bool semantic = line.compare( 0, prefix.size(), prefix ) == 0;
        std::istringstream body( semantic ? line.substr( prefix.size() ) : line );
        TextJsonIn jsin( body );
        const TextJsonObject object = jsin.get_object();
        object.allow_omitted_members();
        const std::string kind = object.get_string( "event" );
        if( !semantic ) {
            CHECK( ( kind.compare( 0, 5, "raid_" ) == 0 || kind == "turn" || diagnostic_kinds.count( kind ) ) );
            raid_diagnostic = raid_diagnostic || kind.compare( 0, 5, "raid_" ) == 0;
            if( diagnostic_kinds.count( kind ) ) {
                retained_diagnostic_kinds.insert( kind );
            }
        } else if( kind == "trace_generation" ) {
            discarded = object.get_int( "recent_records_discarded" );
        } else if( kind == "travel" ) {
            CHECK( object.get_string( "travel_id" ) == "trip-1" );
            travel_states.insert( object.get_string( "state" ) );
        } else if( kind == "surface_descriptor" ) {
            CHECK( object.get_array( "valid_actions" ).size() == 200 );
            current = object.get_string( "frame_id" ) == manager.top()->frame_id;
        } else if( kind == "surface_receipt" && object.get_bool( "accepted" ) ) {
            CHECK( object.get_string( "request_id" ) == "pick-once" );
            CHECK( object.get_string( "action_id" ) == "inventory.pick" );
            accepted_frame = object.get_string( "resulting_frame_id" );
            accepted = true;
        }
    }
    CHECK( discarded > 0 );
    CHECK( travel_states == std::set<std::string> { "active", "hostile_boundary", "progress", "completed_cleared" } );
    CHECK( raid_diagnostic );
    CHECK( retained_diagnostic_kinds == diagnostic_kinds );
    CHECK( current );
    CHECK( accepted );
    CHECK( accepted_frame == manager.top()->frame_id );
    CHECK( fs::file_size( trace ) < 8 * 1024 * 1024 );
    CHECK_FALSE( fs::exists( trace.string() + ".next" ) );
    // A later redraw is current authority; replay must still retain the old
    // successor named by the accepted receipt after that row leaves recent.
    REQUIRE( manager.publish( surface, { { "selection", "redrawn" } }, actions ) );
    CHECK( manager.top()->frame_id != accepted_frame );
    REQUIRE( write_native_semantic_event( "{\"event\":\"travel\",\"run_id\":\"" + run +
                                          "\",\"travel_id\":\"trip-2\",\"receipt_id\":\"trip-2:active\"," +
                                          "\"state\":\"active\",\"destination\":[130,149,0]," +
                                          "\"destination_present\":true,\"destination_cleared\":false}" ) );
    manager.submit_request( request );
    manager.consume_top_request();
    CHECK( dispatched == 1 );
    input.close();
    for( int index = 0; index < 100; ++index ) {
        REQUIRE( write_native_diagnostic_event( "{\"event\":\"turn\",\"run_id\":\"" + run +
                                                "\",\"diagnostic\":\"" + diagnostic + "\"}" ) );
    }
    std::ifstream replayed( trace );
    bool named_successor = false;
    current = false;
    while( std::getline( replayed, line ) ) {
        const std::string prefix = "openclaw_harness_semantic_step: ";
        std::istringstream body( line.compare( 0, prefix.size(), prefix ) == 0 ?
                                 line.substr( prefix.size() ) : line );
        TextJsonIn jsin( body );
        const TextJsonObject object = jsin.get_object();
        object.allow_omitted_members();
        if( object.get_string( "event" ) == "surface_descriptor" ) {
            const std::string frame = object.get_string( "frame_id" );
            named_successor = named_successor || frame == accepted_frame;
            current = current || frame == manager.top()->frame_id;
        } else if( object.get_string( "event" ) == "travel" ) {
            CHECK( object.get_string( "travel_id" ) == "trip-2" );
            CHECK( object.get_string( "state" ) == "active" );
        }
    }
    CHECK( named_successor );
    CHECK( current );
    CHECK( fs::file_size( trace ) < 8 * 1024 * 1024 );
    replayed.close();
    // Actual output failure must recover through a complete replacement, not
    // append behind an incomplete record left by a failed write.
    fs::remove( trace );
    fs::create_directory( trace );
    const bool previous_error = debug_has_error_been_observed();
    CHECK_FALSE( write_native_diagnostic_event( "{\"event\":\"turn\",\"run_id\":\"" + run + "\"}" ) );
    CHECK( debug_has_error_been_observed() );
    if( !previous_error ) {
        debug_reset_error_observed();
    }
    fs::remove( trace );
    {
        std::ofstream broken( trace );
        broken << "openclaw_harness_semantic_step: {";
    }
    REQUIRE( write_native_diagnostic_event( "{\"event\":\"turn\",\"run_id\":\"" + run + "\"}" ) );
    std::ifstream recovered( trace );
    current = accepted = false;
    while( std::getline( recovered, line ) ) {
        const std::string prefix = "openclaw_harness_semantic_step: ";
        std::istringstream body( line.compare( 0, prefix.size(), prefix ) == 0 ?
                                 line.substr( prefix.size() ) : line );
        TextJsonIn jsin( body );
        const TextJsonObject object = jsin.get_object();
        object.allow_omitted_members();
        if( object.get_string( "event" ) == "surface_descriptor" ) {
            CHECK( object.get_array( "valid_actions" ).size() == 200 );
            current = object.get_string( "frame_id" ) == manager.top()->frame_id;
        } else if( object.get_string( "event" ) == "surface_receipt" && object.get_bool( "accepted" ) ) {
            accepted = object.get_string( "request_id" ) == request.request_id;
        }
    }
    CHECK( current );
    CHECK( accepted );
    CHECK( dispatched == 1 );
    const auto before = fs::file_size( trace );
    const bool prior_identity_error = debug_has_error_been_observed();
    CHECK_FALSE(
        write_native_diagnostic_event( "{\"event\":\"npc_alarm\",\"run_id\":\"foreign-run\"}" ) );
    CHECK( fs::file_size( trace ) == before );
    if( !prior_identity_error ) {
        debug_reset_error_observed();
    }
    unsetenv( keys[1].c_str() );
    CHECK_FALSE( write_native_semantic_event( "{\"event\":\"turn\",\"run_id\":\"" + run + "\"}" ) );
    CHECK_FALSE( write_native_diagnostic_event( "{\"event\":\"turn\",\"run_id\":\"" + run + "\"}" ) );
    CHECK( fs::file_size( trace ) == before );
}


TEST_CASE( "native journal rejects invalid records and initial temporary IO before valid retry",
           "[semantic_surface][native_journal_failure_067]" )
{
    namespace fs = std::filesystem;
    const int failure = GENERATE( 0, 1, 2, 3, 4, 5 );
    CAPTURE( failure );
    const fs::path directory = fs::temp_directory_path() /
                               ( "native-journal-failure-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( directory / "config" );
    const fs::path trace = directory / "native.jsonl";
    const fs::path temporary = trace.string() + ".next";
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_TRACE_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    const std::string original_directory = PATH_INFO::user_dir();
    const bool previous_error = debug_has_error_been_observed();
    on_out_of_scope cleanup( [&] {
        for( const auto &entry : previous )
        {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
        deinitDebug();
        PATH_INFO::init_user_dir( original_directory );
        PATH_INFO::set_standard_filenames();
        setupDebug( DebugOutput::std_err );
        if( !previous_error )
        {
            debug_reset_error_observed();
        }
        fs::remove_all( directory );
    } );
    deinitDebug();
    PATH_INFO::init_user_dir( directory.string() + "/" );
    PATH_INFO::set_standard_filenames();
    setupDebug( DebugOutput::file );
    const fs::path log = fs::path( PATH_INFO::debug() );
    const std::string run = directory.filename().string();
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    setenv( keys[2].c_str(), trace.string().c_str(), 1 );
    const std::string good = "{\"event\":\"frame\",\"run_id\":\"" + run +
                             "\",\"frame_id\":\"first-frame\"}";
    std::string record;
    const char *expected_stage = "";
    if( failure == 0 ) {
        record = "{\"event\":@}";
        expected_stage = "json_parse";
    } else if( failure == 1 ) {
        record = "{\"event\":7,\"run_id\":\"" + run + "\"}";
        expected_stage = "record_type field=event";
    } else if( failure == 2 ) {
        record = "{\"event\":\"frame\",\"run_id\":false}";
        expected_stage = "record_type field=run_id";
    } else if( failure == 3 ) {
        record = "{\"event\":\"frame\",\"run_id\":\"foreign\"}";
        expected_stage = "record_run_mismatch";
    } else if( failure == 4 ) {
        // A real temporary-target open failure, not a permission assumption.
        fs::create_directory( temporary );
        record = good;
        expected_stage = "temporary_open";
    } else {
        // First compaction writes successfully, then the actual rename fails.
        fs::create_directory( trace );
        record = good;
        expected_stage = "rename";
    }
    CHECK_FALSE( write_native_semantic_event( record ) );
    CHECK_FALSE( fs::is_regular_file( trace ) );
    deinitDebug();
    std::ifstream errors( log );
    const std::string text( ( std::istreambuf_iterator<char>( errors ) ),
                            std::istreambuf_iterator<char>() );
    CHECK( text.find( std::string( "stage=" ) + expected_stage ) != std::string::npos );
    CHECK( text.find( record ) == std::string::npos );
    if( failure <= 2 ) {
        CHECK( text.find( "line=-1 column=-1" ) == std::string::npos );
    }
    if( failure == 5 ) {
        CHECK( text.find( "error_code=0" ) == std::string::npos );
    }
    errors.close();
    setupDebug( DebugOutput::file );
    if( failure == 4 ) {
        fs::remove( temporary );
    }
    if( failure == 5 ) {
        fs::remove( trace );
    }
    REQUIRE( write_native_semantic_event( good ) );
    CHECK( fs::file_size( trace ) > 0 );
    CHECK_FALSE( fs::exists( temporary ) );
    std::ifstream published( trace );
    std::string row;
    int frames = 0;
    while( std::getline( published, row ) ) {
        const std::string prefix = "openclaw_harness_semantic_step: ";
        REQUIRE( row.compare( 0, prefix.size(), prefix ) == 0 );
        std::istringstream body( row.substr( prefix.size() ) );
        TextJsonIn input( body );
        const auto object = input.get_object();
        object.allow_omitted_members();
        CHECK( object.get_string( "run_id" ) == run );
        if( object.get_string( "event" ) == "frame" ) {
            ++frames;
        }
    }
    // Temporary IO remembers the complete failed row for replacement retry;
    // parser/run refusals never entered retention. No malformed row publishes.
    CHECK( frames == ( failure >= 4 ? 2 : 1 ) );
}

TEST_CASE( "initial HUD World is emitted only after actual journal publication",
           "[semantic_surface][initial_world_publication_retry_067]" )
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() /
                               ( "native-initial-retry-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( directory );
    const fs::path trace = directory / "native.jsonl";
    const fs::path temporary = trace.string() + ".next";
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_TRACE_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    const bool previous_error = debug_has_error_been_observed();
    on_out_of_scope cleanup( [&] {
        for( const auto &entry : previous )
        {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
        if( !previous_error )
        {
            debug_reset_error_observed();
        }
        fs::remove_all( directory );
    } );
    const auto run = directory.filename().string();
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    setenv( keys[2].c_str(), trace.string().c_str(), 1 );
    const auto context = get_default_mode_input_context();
    fs::create_directory( temporary );
    openclaw_harness_semantic_initial_world_frame_if_ready( &context, true, true, true );
    CHECK_FALSE( fs::exists( trace ) );
    fs::remove( temporary );
    // SAME run/context and no gameplay input: failed output must not latch emitted.
    openclaw_harness_semantic_initial_world_frame_if_ready( &context, true, true, true );
    REQUIRE( fs::exists( trace ) );
    const auto bytes = fs::file_size( trace );
    openclaw_harness_semantic_initial_world_frame_if_ready( &context, true, true, true );
    CHECK( fs::file_size( trace ) == bytes );
    std::ifstream published( trace );
    std::string row;
    bool ready = false;
    while( std::getline( published, row ) ) {
        const std::string prefix = "openclaw_harness_semantic_step: ";
        std::istringstream body( row.substr( prefix.size() ) );
        TextJsonIn input( body );
        const auto object = input.get_object();
        object.allow_omitted_members();
        ready = ready || ( object.get_string( "event" ) == "frame" &&
                           object.get_string( "producer", "" ) == "hud_world_ready" &&
                           object.get_bool( "initial_world_ready", false ) );
    }
    CHECK( ready );
}


TEST_CASE( "rejected receipt and travel types cannot alter later journal retention",
           "[semantic_surface][native_journal_rejected_retention_067]" )
{
    namespace fs = std::filesystem;
    const int invalid = GENERATE( 0, 1, 2, 3 );
    CAPTURE( invalid );
    const fs::path directory = fs::temp_directory_path() /
                               ( "native-rejected-retention-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( directory );
    const fs::path trace = directory / "native.jsonl";
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_TRACE_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    const bool previous_error = debug_has_error_been_observed();
    on_out_of_scope cleanup( [&] {
        for( const auto &entry : previous )
        {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
        if( !previous_error )
        {
            debug_reset_error_observed();
        }
        fs::remove_all( directory );
    } );
    const auto run = directory.filename().string();
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    setenv( keys[2].c_str(), trace.string().c_str(), 1 );
    const std::string base = "\"run_id\":\"" + run + "\",";
    REQUIRE( write_native_semantic_event( "{" + base +
                                          "\"event\":\"surface_descriptor\",\"frame_id\":\"frame-0\"}" ) );
    REQUIRE( write_native_semantic_event( "{" + base +
                                          "\"event\":\"surface_receipt\",\"request_id\":\"accepted-once\","
                                          "\"accepted\":true,\"resulting_frame_id\":\"frame-0\"}" ) );
    for( const auto state : {
             "active", "hostile_boundary", "progress"
         } ) {
        REQUIRE( write_native_semantic_event( "{" + base +
                                              "\"event\":\"travel\",\"travel_id\":\"current-trip\","
                                              "\"state\":\"" + state + "\"}" ) );
    }
    std::string rejected;
    if( invalid == 0 ) {
        rejected = "{" + base + "\"event\":\"surface_receipt\",\"request_id\":\"rejected-record\","
                   "\"accepted\":true,\"resulting_frame_id\":7}";
    } else if( invalid == 1 ) {
        rejected = "{" + base + "\"event\":\"surface_receipt\",\"request_id\":\"rejected-record\","
                   "\"accepted\":7,\"resulting_frame_id\":\"frame-0\"}";
    } else if( invalid == 2 ) {
        rejected = "{" + base + "\"event\":\"travel\",\"travel_id\":\"rejected-record\",\"state\":7}";
    } else {
        rejected = "{" + base + "\"event\":\"travel\",\"travel_id\":7,\"state\":\"rejected-record\"}";
    }
    REQUIRE_FALSE( write_native_semantic_event( rejected ) );
    // Evict the earlier rows from the existing 256-record recent window so
    // retry tests the authority pins, not a surviving diagnostic copy.
    for( int index = 0; index <= 256; ++index ) {
        REQUIRE( write_native_diagnostic_event( "{" + base +
                                                "\"event\":\"turn\",\"index\":" + std::to_string( index ) + "}" ) );
    }
    // Use the already-supported real output failure/replacement route to
    // compel compaction, without calling a private journal or extra parser.
    fs::remove( trace );
    fs::create_directory( trace );
    REQUIRE_FALSE( write_native_diagnostic_event( "{" + base + "\"event\":\"turn\"}" ) );
    fs::remove( trace );
    REQUIRE( write_native_semantic_event( "{" + base +
                                          "\"event\":\"surface_descriptor\",\"frame_id\":\"frame-1\"}" ) );
    std::ifstream published( trace );
    std::string row;
    bool accepted = false;
    bool current_frame = false;
    std::set<std::string> travel_states;
    while( std::getline( published, row ) ) {
        CHECK( row.find( "rejected-record" ) == std::string::npos );
        const std::string prefix = "openclaw_harness_semantic_step: ";
        std::istringstream body( row.compare( 0, prefix.size(), prefix ) == 0 ?
                                 row.substr( prefix.size() ) : row );
        TextJsonIn input( body );
        const auto object = input.get_object();
        object.allow_omitted_members();
        CHECK( object.get_string( "run_id" ) == run );
        const auto kind = object.get_string( "event" );
        if( kind == "surface_descriptor" ) {
            current_frame = current_frame || object.get_string( "frame_id" ) == "frame-1";
        } else if( kind == "surface_receipt" ) {
            if( object.get_string( "request_id" ) == "accepted-once" ) {
                CHECK( object.get_bool( "accepted" ) );
                CHECK( object.get_string( "resulting_frame_id" ) == "frame-0" );
                accepted = true;
            }
        } else if( kind == "travel" ) {
            CHECK( object.get_string( "travel_id" ) == "current-trip" );
            travel_states.insert( object.get_string( "state" ) );
        }
    }
    CHECK( accepted );
    CHECK( current_frame );
    CHECK( travel_states == std::set<std::string> { "active", "hostile_boundary", "progress" } );
    CHECK_FALSE( fs::exists( trace.string() + ".next" ) );
    std::cout << "R067_REJECTED_RETENTION invalid=" << invalid
              << " accepted=" << accepted << " current_frame=" << current_frame
              << " retained_travel_states=" << travel_states.size() << '\n';
}

TEST_CASE( "debug file is bounded during one long running process", "[debug][native_log_window]" )
{
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() /
                               ( "caol-debug-window-" + std::to_string(
                                     std::chrono::steady_clock::now().time_since_epoch().count() ) );
    fs::create_directories( directory / "config" );
    const std::string original = PATH_INFO::user_dir();
    on_out_of_scope cleanup( [&] {
        deinitDebug();
        PATH_INFO::init_user_dir( original );
        PATH_INFO::set_standard_filenames();
        setupDebug( DebugOutput::std_err );
        fs::remove_all( directory );
    } );
    deinitDebug();
    PATH_INFO::init_user_dir( directory.string() + "/" );
    PATH_INFO::set_standard_filenames();
    setupDebug( DebugOutput::file );
    const fs::path log = fs::path( PATH_INFO::debug() );
    for( int index = 0; index < 100; ++index ) {
        DebugLog( D_INFO, D_MAIN ) << std::string( 100 * 1024, 'd' );
    }
    DebugLog( D_INFO, D_MAIN ) << "latest-current-diagnostic";
    deinitDebug();
    REQUIRE( fs::exists( log ) );
    CHECK( fs::file_size( log ) <= 8 * 1024 * 1024 );
    CHECK_FALSE( fs::exists( log.string() + ".prev" ) );
    CHECK_FALSE( fs::exists( log.string() + ".next" ) );
    std::ifstream input( log );
    const std::string retained( ( std::istreambuf_iterator<char>( input ) ),
                                std::istreambuf_iterator<char>() );
    CHECK( retained.find( "Earlier diagnostic log window discarded" ) != std::string::npos );
    CHECK( retained.find( "latest-current-diagnostic" ) != std::string::npos );
}


extern "C" int flushinp();

#if !defined(TILES)
TEST_CASE( "avatar activity driver publishes its actual tired travel decision",
           "[semantic_surface][activity_driver_lifetime_067]" )
{
    const bool enclosing = GENERATE( false, true );
    const int choice = GENERATE( 1, 2, 3, 4 ); // 4 is native Escape from the tired query.
    const bool canceled = choice == 2 || choice == 4;
    const bool escaped = choice == 4;
    CAPTURE( enclosing, choice );
    override_option autosave( "AUTOSAVE", "false" );
    override_option llm( "LLM_INTENT_ENABLE", "false" );
    clear_avatar();
    clear_map();
    clear_npcs();
    if( canceled ) {
        // Set up the ordinary native turn mode through registered save/load;
        // handle_action is private and must be reached by its actual caller.
        REQUIRE( world_generator->active_world );
        const auto world = world_generator->active_world->world_name;
        REQUIRE( g->save() );
        REQUIRE( turn_handler::cleanup_at_end() );
        world_generator->set_active_world( nullptr );
        REQUIRE( g->load( world ) );
        clear_avatar();
        clear_map();
        clear_npcs();
    }
    avatar &u = get_avatar();
    map &here = get_map();
    u.setpos( here, tripoint_bub_ms( 60, 60, 0 ), false );
    u.set_stamina( u.get_stamina_max() / 4 );
    u.set_moves( 100 );
    const auto current = u.pos_abs_omt();
    u.omt_path = { current + point::east, current };
    u.assign_activity( player_activity( activity_id( "ACT_TRAVELLING" ) ) );
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    const std::string run = "activity-driver-" +
                            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    unsetenv( keys[2].c_str() );
    on_out_of_scope restore( [&]() {
        for( const auto &entry : previous ) {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
        u.cancel_activity();
        u.clear_destination();
        u.omt_path.clear();
    } );
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    }
    on_out_of_scope close_terminal( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
    scoped_imgui_context imgui;
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    ui_adaptor fixture_root;
    fixture_root.on_redraw( []( ui_adaptor & ) {} );
    ui_manager::redraw(); // native IMTUI frame/fonts before uilist::calc_data
    semantic_surface_manager nested( run );
    semantic_surface_manager &manager = enclosing ? nested :
                                        openclaw_harness_semantic_surface_manager();
    std::optional<semantic_surface_manager_session> session;
    if( enclosing ) {
        session.emplace( manager );
    }
    std::vector<semantic_action_receipt> receipts;
    std::vector<semantic_action_receipt> world_receipts;
    int menus = 0;
    int world_frames = 0;
    semantic_action_request selected;
    std::optional<semantic_surface_descriptor> successor;
    semantic_surface_descriptor old_overmap;
    {
        // The native action owner has already returned before either activity
        // driver executes. Its old close is never consent to the new decision.
        semantic_surface_scope overmap( manager, "overmap", "Overmap", {},
        { { "overmap.close", "", "Close", true } } );
        old_overmap = *manager.top();
    }
    manager.set_receipt_observer( [&]( const auto & receipt ) {
        if( receipt.request_id.find( "ordinary-world-pause-" ) == 0 ) {
            world_receipts.push_back( receipt );
        } else {
            receipts.push_back( receipt );
        }
    } );
    on_out_of_scope remove_observers( [&]() {
        manager.set_descriptor_observer( {} );
        manager.set_receipt_observer( {} );
        if( enclosing ) {
            openclaw_harness_semantic_surface_manager().set_descriptor_observer( {} );
            openclaw_harness_semantic_surface_manager().set_receipt_observer( {} );
        }
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & d ) {
        if( d.kind == "world" ) {
            ++world_frames;
            const auto action = std::find_if( d.valid_actions.begin(), d.valid_actions.end(),
            []( const auto & a ) {
                return a.id == "world.pause";
            } );
            REQUIRE( action != d.valid_actions.end() );
            REQUIRE( action->enabled );
            REQUIRE( openclaw_harness_semantic_surface_manager().submit_request( { run, d.surface_id, d.frame_id, "ordinary-world-pause-" + std::to_string( world_frames ),
                     action->id, action->stable_id, {} } ) );
            return;
        }
        if( d.kind == "activity_wait" ) {
            successor = d;
        }
        if( d.kind != "menu" ) {
            return;
        }
        ++menus;
        CHECK( active_semantic_surface_manager() == &manager );
        CHECK( d.payload.at( "text" ).find( "You struggle to continue" ) != std::string::npos );
        CHECK( d.frame_id != old_overmap.frame_id );
        REQUIRE( manager.submit_request( { run, old_overmap.surface_id, old_overmap.frame_id,
                                           "stale-close", "overmap.close", "", {} } ) );
        if( escaped ) {
            return;
        }
        const std::string label = choice == 1 ? "Continue after a break." :
                                  choice == 2 ? "Maybe later." : "Finish it.";
        const auto action = std::find_if( d.valid_actions.begin(), d.valid_actions.end(),
        [&]( const auto & a ) {
            return a.id == "menu.choose" && a.label == label;
        } );
        REQUIRE( action != d.valid_actions.end() );
        CHECK( action->enabled );
        selected = { run, d.surface_id, d.frame_id, "tired-choice", action->id, action->stable_id, {} };
        REQUIRE( manager.submit_request( selected ) );
    } );
    if( enclosing ) {
        // Ordinary handle_action owns the global manager even when the earlier
        // activity driver preserved an enclosing modal manager.
        auto &world_manager = openclaw_harness_semantic_surface_manager();
        world_manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & d ) {
            if( d.kind == "world" ) {
                ++world_frames;
                const auto action = std::find_if( d.valid_actions.begin(), d.valid_actions.end(),
                []( const auto & a ) {
                    return a.id == "world.pause";
                } );
                REQUIRE( action != d.valid_actions.end() );
                REQUIRE( action->enabled );
                REQUIRE( openclaw_harness_semantic_surface_manager().submit_request( { run, d.surface_id, d.frame_id, "ordinary-world-pause-" + std::to_string( world_frames ),
                         action->id, action->stable_id, {} } ) );
                return;
            }
        } );
        world_manager.set_receipt_observer( [&]( const auto & receipt ) {
            world_receipts.push_back( receipt );
        } );
    }
    // Disposable curses fixture only: makes the pre-fix missing-menu control
    // terminate honestly, rather than hanging. Semantic dispatch takes priority.
    REQUIRE( ungetch( escaped ? 27 : 'm' ) != -1 );
    process_avatar_activity_turn( u ); // shared by pre-input and post-action loops
    flushinp();
    CHECK( active_semantic_surface_manager() == ( enclosing ? &manager : nullptr ) );
    CHECK( menus == 1 );
    if( choice == 1 ) {
        CHECK( u.activity.id() == activity_id( "ACT_WAIT_STAMINA" ) );
    } else {
        CHECK_FALSE( u.activity );
    }
    if( canceled ) {
        CHECK_FALSE( u.has_destination() );
        CHECK_FALSE( u.has_destination_activity() );
        CHECK_FALSE( u.is_auto_moving() );
        CHECK( u.get_next_auto_move_direction() == ACTION_NULL );
    } else {
        CHECK( u.has_destination() );
    }
    if( menus == 0 ) {
        return;    // Preserve failure without manufacturing a choice.
    }
    const size_t menu_receipts = escaped ? 1 : 2;
    REQUIRE( receipts.size() == menu_receipts );
    CHECK_FALSE( receipts.front().accepted );
    CHECK( receipts.front().request_id == "stale-close" );
    if( !escaped ) {
        CHECK( receipts.back().accepted );
        CHECK( receipts.back().request_id == "tired-choice" );
        CHECK( receipts.back().consuming_frame_id == selected.frame_id );
    }
    // A terminal uilist choice receipts on pop; Escape has no choice receipt.
    // Continuing choices publish the resulting blocking owner; explicit
    // cancellation leaves no activity or auto-move owner to publish here.
    handle_key_blocking_activity();
    REQUIRE( receipts.size() == menu_receipts );
    if( canceled ) {
        CHECK_FALSE( successor );
        CHECK_FALSE( u.has_destination() );
        CHECK( u.get_next_auto_move_direction() == ACTION_NULL );
    } else {
        REQUIRE( successor );
        CHECK( successor->kind == "activity_wait" );
        CHECK( successor->frame_id != selected.frame_id );
    }
    const auto activity_after = u.activity.id();
    const auto route_after = u.omt_path;
    if( canceled ) {
        const tripoint_abs_ms position_after_cancel = u.pos_abs();
        // Exercise the next real World input owner, without issuing another
        // travel command.  Pause is a native nonmoving action used only to
        // let the caller return after proving no route is consumed.
        u.set_moves( 100 );
        CHECK_FALSE( g->do_turn() );
        // The next genuine owner publication settles the deferred pause
        // receipt; a dequeue alone must not be counted as acceptance.
        u.set_moves( 100 );
        CHECK_FALSE( g->do_turn() );
        CHECK( world_frames == 2 );
        CHECK( menus == 1 );
        CHECK( u.pos_abs() == position_after_cancel );
        CHECK_FALSE( u.has_destination() );
        CHECK( u.get_next_auto_move_direction() == ACTION_NULL );
        CHECK( u.omt_path == route_after ); // Strategic route memory remains inert.
        CHECK( std::count_if( world_receipts.begin(), world_receipts.end(), []( const auto & r ) {
            return r.accepted && r.request_id == "ordinary-world-pause-1";
        } ) == 1 );
        REQUIRE( receipts.size() == menu_receipts );
        CHECK( active_semantic_surface_manager() == ( enclosing ? &manager : nullptr ) );
    }
    if( !escaped ) {
        CHECK_FALSE( manager.submit_request( selected ) );
        CHECK( receipts.size() == menu_receipts + 1 );
        CHECK( receipts.back().request_id == "tired-choice" ); // cached, no second native choice
    }
    CHECK( u.activity.id() == activity_after );
    CHECK( u.omt_path == route_after );
    CHECK( menus == 1 );
}

TEST_CASE( "avatar activity driver leaves harness-off input unbound",
           "[semantic_surface][activity_driver_lifetime_067]" )
{
    clear_avatar();
    clear_map();
    avatar &u = get_avatar();
    u.setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ), false );
    u.set_stamina( u.get_stamina_max() / 4 );
    u.set_moves( 100 );
    u.omt_path = { u.pos_abs_omt() + point::east, u.pos_abs_omt() };
    u.assign_activity( player_activity( activity_id( "ACT_TRAVELLING" ) ) );
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID", "OPENCLAW_HARNESS_SEMANTIC_RUN_ID" };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
        unsetenv( key.c_str() );
    }
    on_out_of_scope restore( [&]() {
        for( const auto &entry : previous ) {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            }
        }
        u.cancel_activity();
        u.clear_destination();
        u.omt_path.clear();
    } );
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    }
    on_out_of_scope close_terminal( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
    scoped_imgui_context imgui;
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    ui_adaptor fixture_root;
    fixture_root.on_redraw( []( ui_adaptor & ) {} );
    ui_manager::redraw();
    CHECK( active_semantic_surface_manager() == nullptr );
    REQUIRE( ungetch( 'm' ) != -1 );
    process_avatar_activity_turn( u );
    CHECK( active_semantic_surface_manager() == nullptr );
    CHECK_FALSE( u.activity );
    flushinp();
}
#endif

TEST_CASE( "NPC pulp selection reconciles ordinary and residual destination states",
           "[npc][pulp_destination_diagnosis_067]" )
{
    const std::string mode = GENERATE( "corpse", "empty", "reachable_destination",
                                       "blocked_destination",
                                       "danger", "high_backlog" );
    CAPTURE( mode );
    clear_avatar();
    clear_map();
    clear_npcs();
    set_time_to_day();
    avatar &u = get_avatar();
    u.setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ), false );
    npc &actor = spawn_npc( { 61, 60 }, "test_talker" );
    clear_character( actor );
    actor.set_attitude( NPCATT_FOLLOW );
    actor.rules.set_flag( ally_rule::allow_pulp );
    actor.set_stamina( actor.get_stamina_max() );
    const auto corpse_pos = actor.pos_bub();
    actor.pulp_location = actor.pos_abs(); // ordinary remembered corpse target
    if( mode != "empty" ) {
        get_map().add_item_or_charges( corpse_pos, item::make_corpse( mtype_id( "mon_zombie" ) ) );
    }
    // Discover and execute through npc::move, not a direct activity/execute_action call.
    actor.set_moves( 100 );
    actor.move();
    CAPTURE( actor.activity.id().str(), actor.get_moves() );
    if( mode == "reachable_destination" || mode == "blocked_destination" ) {
        const auto destination = corpse_pos + point( 3, 0 );
        if( mode == "blocked_destination" ) {
            get_map().ter_set( destination, ter_id( "t_rock" ) );
        }
        actor.set_destination( { destination } );
        actor.update_path( destination );
        const bool viable = !actor.path.empty();
        std::cout << "pulp_destination mode=" << mode << " route_viable=" << viable << '\n';
        CHECK( viable == ( mode == "reachable_destination" ) );
    }
    if( mode == "danger" ) {
        spawn_test_monster( "mon_zombie", corpse_pos + point::south );
    }
    restore_on_out_of_scope<bool> restore_debug( debug_mode );
    debug_mode = true;
    int zero_move_calls = 0;
    const auto initial_turn = calendar::turn;
    for( int iteration = 0; iteration < 12; ++iteration ) {
        // A same-pass no-op retains its move budget and clock. Real expenditure
        // starts the next turn; this distinguishes pulping work from guard spins.
        if( iteration == 0 || actor.get_moves() < 100 ) {
            actor.set_moves( mode == "high_backlog" ? 6000 : 100 );
            calendar::turn += 1_turns;
        }
        const int before = actor.get_moves();
        const auto activity_before = actor.activity.id().str();
        const auto left_before = actor.activity.moves_left;
        Messages::clear_messages();
        actor.move();
        if( actor.get_moves() == before ) {
            ++zero_move_calls;
        }
        std::cout << "pulp_control mode=" << mode << " iteration=" << iteration
                  << " turn=" << to_turns<int>( calendar::turn - initial_turn )
                  << " moves=" << before << "->" << actor.get_moves()
                  << " activity=" << activity_before << "->" << actor.activity.id().str()
                  << " moves_left=" << left_before << "->" << actor.activity.moves_left
                  << " destination=" << actor.has_destination()
                  << " destination_activity=" << actor.has_destination_activity()
                  << " path_size=" << actor.path.size() << " can_pulp=" << actor.can_do_pulp() << '\n';
        for( const auto &message : Messages::recent_messages( 40 ) ) {
            if( message.second.find( "NPC didn't use its moves." ) != std::string::npos ||
                message.second.find( "address_needs " ) != std::string::npos ) {
                std::cout << "pulp_selection " << message.second << '\n';
            }
        }
    }
    CAPTURE( zero_move_calls, actor.activity.id().str(), actor.has_destination(),
             actor.get_ai_danger() );
    const bool residual_destination = mode == "blocked_destination" || mode == "reachable_destination";
    if( residual_destination ) {
        // Reproduces the existing defect; this passing mechanism control is
        // explicitly not a gameplay fix or historical Casey attribution.
        CHECK( zero_move_calls == 12 );
        CHECK( actor.activity.id() == activity_id( "ACT_PULP" ) );
        CHECK( actor.has_destination() );
    } else {
        CHECK( zero_move_calls < 9 );
    }
    const std::string warning = capture_debugmsg_during( [&]() {
        process_monsters_and_npcs_turn_for_test();
    } );
    std::cout << "pulp_guard mode=" << mode << " warning=" << warning
              << " suspended=" << actor.has_effect( efftype_id( "npc_suspend" ) ) << '\n';
    if( residual_destination ) {
        CHECK( actor.has_effect( efftype_id( "npc_suspend" ) ) );
        CHECK( warning.find( "entered infinite loop" ) != std::string::npos );
        CHECK( warning.find( "ACT_PULP" ) != std::string::npos );
    } else {
        CHECK_FALSE( actor.has_effect( efftype_id( "npc_suspend" ) ) );
        CHECK( warning.empty() );
    }
    clear_npcs();
}

#if !defined(TILES)
TEST_CASE( "World movement mode opens its native choices and restores ordinary input",
           "[semantic_surface][movement_mode_067]" )
{
    const std::string choice = GENERATE( "walk", "run", "crouch", "prone", "cycle", "cancel" );
    const bool enclosing = GENERATE( false, true );
    CAPTURE( choice, enclosing );
    override_option autosave( "AUTOSAVE", "false" );
    override_option llm( "LLM_INTENT_ENABLE", "false" );
    clear_avatar();
    clear_map();
    clear_npcs();
    REQUIRE( world_generator->active_world );
    const auto world = world_generator->active_world->world_name;
    REQUIRE( g->save() );
    REQUIRE( turn_handler::cleanup_at_end() );
    world_generator->set_active_world( nullptr );
    REQUIRE( g->load( world ) );
    clear_avatar();
    clear_map();
    clear_npcs();
    avatar &u = get_avatar();
    u.setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ), false );
    u.set_walk_mode_desired();
    const auto position = u.pos_abs();
    input_context unregistered( "movement-mode-unregistered" );
    const auto unavailable = openclaw_harness_world_actions( unregistered );
    CHECK( std::none_of( unavailable.begin(), unavailable.end(), []( const auto & a ) {
        return a.first == "world.movement_mode";
    } ) );
    const auto available = openclaw_harness_world_actions( get_default_mode_input_context() );
    CHECK( std::count_if( available.begin(), available.end(), []( const auto & a ) {
        return a.first == "world.movement_mode" && a.second == "open_movement";
    } ) == 1 );
    const std::vector<std::string> keys = { "OPENCLAW_HARNESS_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_RUN_ID",
                                            "OPENCLAW_HARNESS_SEMANTIC_REQUEST_PATH"
                                          };
    std::map<std::string, std::optional<std::string>> previous;
    for( const auto &key : keys ) {
        const char *value = std::getenv( key.c_str() );
        previous[key] = value ? std::optional<std::string>( value ) : std::nullopt;
    }
    const std::string run = "movement-mode-" +
                            std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() );
    setenv( keys[0].c_str(), run.c_str(), 1 );
    setenv( keys[1].c_str(), run.c_str(), 1 );
    unsetenv( keys[2].c_str() );
    on_out_of_scope restore_environment( [&]() {
        for( const auto &entry : previous ) {
            if( entry.second ) {
                setenv( entry.first.c_str(), entry.second->c_str(), 1 );
            } else {
                unsetenv( entry.first.c_str() );
            }
        }
    } );
    const bool owns_terminal = !catacurses::stdscr;
    if( owns_terminal ) {
        catacurses::init_interface();
        catacurses::resizeterm();
    }
    on_out_of_scope close_terminal( [&]() {
        if( owns_terminal ) {
            catacurses::endwin();
        }
    } );
    scoped_imgui_context imgui;
    restore_on_out_of_scope<bool> restore_test_mode( test_mode );
    test_mode = false;
    ui_adaptor root;
    root.on_redraw( []( ui_adaptor & ) {} );
    ui_manager::redraw();
    semantic_surface_manager &manager = openclaw_harness_semantic_surface_manager();
    std::optional<semantic_surface_manager_session> session;
    if( enclosing ) {
        session.emplace( manager );
    }
    std::vector<semantic_action_receipt> receipts;
    std::optional<semantic_surface_descriptor> original_world;
    semantic_action_request selected;
    int worlds = 0;
    int menus = 0;
    manager.set_receipt_observer( [&]( const auto & r ) {
        receipts.push_back( r );
    } );
    on_out_of_scope remove_observers( [&]() {
        manager.set_descriptor_observer( {} );
        manager.set_receipt_observer( {} );
    } );
    manager.set_descriptor_observer( [&]( const semantic_surface_descriptor & d ) {
        if( d.kind == "world" ) {
            ++worlds;
            const std::string action_id = worlds == 1 ? "world.movement_mode" : "world.pause";
            const auto action = std::find_if( d.valid_actions.begin(), d.valid_actions.end(),
            [&]( const auto & a ) {
                return a.id == action_id;
            } );
            REQUIRE( action != d.valid_actions.end() );
            REQUIRE( action->enabled );
            if( worlds == 1 ) {
                original_world = d;
            } else {
                CHECK( d.surface_id != original_world->surface_id );
            }
            REQUIRE( manager.submit_request( { run, d.surface_id, d.frame_id,
                                               "mode-world-" + std::to_string( worlds ), action->id,
                                               action->stable_id, {} } ) );
        } else if( d.kind == "menu" ) {
            ++menus;
            CHECK( active_semantic_surface_manager() == &manager );
            CHECK( d.payload.at( "text" ) == "Change to which movement mode?" );
            REQUIRE( original_world );
            // Retired World authority cannot become another movement request
            // while the real native uilist owns input.
            REQUIRE( manager.submit_request( { run, original_world->surface_id,
                                               original_world->frame_id, "stale-mode-world",
                                               "world.movement_mode", "", {} } ) );
            for( const auto &mode : move_modes_by_speed() ) {
                const auto entry = std::find_if( d.valid_actions.begin(), d.valid_actions.end(),
                [&]( const auto & a ) {
                    return a.id == "menu.choose" && a.label.find( mode->name() ) == 0;
                } );
                REQUIRE( entry != d.valid_actions.end() );
                CHECK( entry->enabled == u.can_switch_to( mode ) );
            }
            const std::string label = choice == "cycle" ? "Cycle move mode" :
                                      choice == "cancel" ? "" : move_mode_id( choice )->name();
            const auto entry = std::find_if( d.valid_actions.begin(), d.valid_actions.end(),
            [&]( const auto & a ) {
                return choice == "cancel" ? a.id == "menu.cancel" :
                       a.id == "menu.choose" && a.label.find( label ) == 0;
            } );
            REQUIRE( entry != d.valid_actions.end() );
            REQUIRE( entry->enabled );
            selected = { run, d.surface_id, d.frame_id, "mode-choice", entry->id,
                         entry->stable_id, {}
                       };
            REQUIRE( manager.submit_request( selected ) );
        }
    } );
    u.set_moves( 100 );
    CHECK_FALSE( g->do_turn() );
    u.set_moves( 100 );
    CHECK_FALSE( g->do_turn() );
    REQUIRE( menus == 1 );
    CHECK( worlds >= 2 );
    CHECK( u.pos_abs() == position );
    CHECK_FALSE( u.activity );
    CHECK_FALSE( u.has_destination() );
    if( choice == "cycle" ) {
        CHECK( u.get_desired_move_mode() != move_mode_id( "walk" ) );
    } else {
        CHECK( u.get_desired_move_mode() == move_mode_id( choice == "cancel" ? "walk" : choice ) );
    }
    CHECK( u.current_movement_mode() == u.get_desired_move_mode() );
    CHECK( std::count_if( receipts.begin(), receipts.end(), []( const auto & r ) {
        return r.accepted && r.request_id == "mode-world-1";
    } ) == 1 );
    CHECK( std::count_if( receipts.begin(), receipts.end(), []( const auto & r ) {
        return r.accepted && r.request_id == "mode-choice";
    } ) == 1 );
    CHECK( std::count_if( receipts.begin(), receipts.end(), []( const auto & r ) {
        return !r.accepted && r.request_id == "stale-mode-world";
    } ) == 1 );
    CHECK( std::count_if( receipts.begin(), receipts.end(), []( const auto & r ) {
        return r.accepted && r.request_id == "mode-world-2";
    } ) == 1 );
    CHECK_FALSE( manager.submit_request( selected ) );
    CHECK( menus == 1 );
    CHECK( active_semantic_surface_manager() == ( enclosing ? &manager : nullptr ) );
    flushinp();
}
#endif
