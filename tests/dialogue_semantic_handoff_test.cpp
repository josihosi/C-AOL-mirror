#include <optional>
#include <string>
#include <vector>

#include "cata_catch.h"
#include "semantic_surface.h"

TEST_CASE( "committed dialogue effects do not expose retiring dialogue or World frames",
           "[semantic_surface][dialogue_handoff]" )
{
    bool opens_rules = false;
    SECTION( "next dialogue topic" ) {}
    SECTION( "rules effect then conversation ends" ) {
        opens_rules = true;
    }
    semantic_surface_manager manager( "dialogue-run" );
    std::vector<semantic_surface_descriptor> descriptors;
    std::vector<semantic_action_receipt> receipts;
    manager.set_descriptor_observer( [&descriptors]( const semantic_surface_descriptor & descriptor ) {
        descriptors.push_back( descriptor );
    } );
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    std::optional<semantic_surface_scope> world;
    world.emplace( manager, "world", "World" );
    {
        semantic_surface_scope dialogue( manager, "dialogue", "Katharina", {}, {
            { "dialogue.choose", "response-1", "Continue", true }
        }, []( const semantic_action_request & ) {
            return semantic_action_dispatch_result{ true, "", "", true, false };
        } );
        const auto frame = *manager.top();
        REQUIRE( manager.submit_request( { "dialogue-run", frame.surface_id, frame.frame_id,
                                           "choose-1", "dialogue.choose", "response-1", {} } ) );
        REQUIRE( dialogue.consume_request() );
        REQUIRE( receipts.size() == 1 );
        CHECK( receipts.front().resulting_frame_id.empty() );
        REQUIRE( manager.withhold_parent_authority_until_recreated( dialogue.surface_id() ) );
        if( opens_rules ) {
            {
                semantic_surface_scope rules( manager, "npc_rules_menu", "Rules", {}, {
                    { "menu.cancel", "", "Done", true }
                }, []( const semantic_action_request & ) {
                    return semantic_action_dispatch_result{ true, "", "" };
                } );
                const auto rules_frame = *manager.top();
                REQUIRE( manager.submit_request( { "dialogue-run", rules_frame.surface_id,
                                                   rules_frame.frame_id, "close-rules", "menu.cancel", std::nullopt, {} } ) );
                REQUIRE( rules.consume_request() );
            }
            CHECK_FALSE( manager.top() );
            CHECK( receipts.size() == 1 );
        }
    }
    CHECK_FALSE( manager.top() );
    REQUIRE( descriptors.size() == ( opens_rules ? 3 : 2 ) );
    if( opens_rules ) {
        world.reset();
    }
    semantic_surface_scope next( manager, opens_rules ? "world" : "dialogue",
                                 opens_rules ? "World" : "Katharina" );
    REQUIRE( manager.top() );
    CHECK( descriptors.back().surface_id == next.surface_id() );
    CHECK( descriptors.size() == ( opens_rules ? 4 : 3 ) );
    if( opens_rules ) {
        REQUIRE( receipts.size() == 2 );
        CHECK( receipts.back().request_id == "close-rules" );
        CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
    }
}

TEST_CASE( "nested retiring dialogue and item owners preserve their own successor boundaries",
           "[semantic_surface][dialogue_handoff]" )
{
    semantic_surface_manager manager( "nested-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt & receipt ) {
        receipts.push_back( receipt );
    } );
    semantic_surface_scope world( manager, "world", "World" );
    {
        semantic_surface_scope dialogue( manager, "dialogue", "Katharina" );
        REQUIRE( manager.withhold_parent_authority_until_recreated( dialogue.surface_id() ) );
        {
            semantic_surface_scope item( manager, "inventory_item_menu", "lighter" );
            REQUIRE( manager.withhold_parent_authority_until_recreated( item.surface_id() ) );
            {
                semantic_surface_scope direction( manager, "direction", "Light where?", {}, {
                    { "direction.choose", "north", "North", true }
                }, []( const semantic_action_request & ) {
                    return semantic_action_dispatch_result{ true, "", "" };
                } );
                const auto frame = *manager.top();
                REQUIRE( manager.submit_request( { "nested-run", frame.surface_id, frame.frame_id,
                                                   "light", "direction.choose", "north", {} } ) );
                REQUIRE( direction.consume_request() );
            }
            CHECK_FALSE( manager.top() );
            CHECK( receipts.empty() );
        }
        CHECK_FALSE( manager.top() );
        {
            semantic_surface_scope recreated_item( manager, "inventory_item_menu", "lighter" );
            REQUIRE( receipts.size() == 1 );
            CHECK( receipts.back().resulting_frame_id == manager.top()->frame_id );
            const auto item_frame = *manager.top();
            // An ordinary nested detail view still returns to its active
            // item owner, despite the retired dialogue further down-stack.
            {
                semantic_surface_scope details( manager, "item_info", "lighter details" );
            }
            REQUIRE( manager.top() );
            CHECK( manager.top()->surface_id == item_frame.surface_id );
        }
        CHECK_FALSE( manager.top() );
    }
    CHECK_FALSE( manager.top() );
    semantic_surface_scope next_dialogue( manager, "dialogue", "Katharina" );
    REQUIRE( manager.top() );
    CHECK( manager.top()->surface_id == next_dialogue.surface_id() );
}


TEST_CASE( "deferred rules close survives World retirement without repeating its effect",
           "[semantic_surface][dialogue_handoff]" )
{
    semantic_surface_manager manager( "retirement-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt &receipt ) {
        receipts.push_back( receipt );
    } );
    int effects = 0;
    std::optional<semantic_surface_scope> world;
    world.emplace( manager, "world", "World" );
    std::optional<semantic_surface_scope> dialogue;
    dialogue.emplace( manager, "dialogue", "Companion" );
    REQUIRE( manager.withhold_parent_authority_until_recreated( dialogue->surface_id() ) );
    std::optional<semantic_surface_scope> rules;
    rules.emplace( manager, "npc_rules_menu", "Rules", std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> { { "menu.cancel", "", "Done", true } },
    [&effects]( const semantic_action_request & ) {
        ++effects;
        return semantic_action_dispatch_result{ true, "", "" };
    } );
    const auto frame = *manager.top();
    const semantic_action_request close { manager.run_id(), frame.surface_id, frame.frame_id,
                                         "done", "menu.cancel", std::nullopt, {} };
    REQUIRE( manager.submit_request( close ) );
    REQUIRE( rules->consume_request() );
    REQUIRE( effects == 1 );
    REQUIRE( receipts.empty() );
    rules.reset();
    dialogue.reset();
    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );
    world.reset();
    CHECK_FALSE( manager.top() );
    CHECK( receipts.empty() );

    semantic_surface_scope next_world( manager, "world", "World" );
    REQUIRE( manager.top() );
    REQUIRE( receipts.size() == 1 );
    const auto successor = manager.top()->frame_id;
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().request_id == "done" );
    CHECK( receipts.front().resulting_frame_id == successor );
    REQUIRE( next_world.publish( { { "tick", "next" } }, {} ) );
    CHECK( receipts.size() == 1 );
    CHECK_FALSE( manager.submit_request( close ) );
    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.back().resulting_frame_id == successor );
    CHECK_FALSE( next_world.consume_request() );
    CHECK( effects == 1 );
}

TEST_CASE( "terminal receipts remain once-only with or without a retiring owner",
           "[semantic_surface][dialogue_handoff]" )
{
    bool immediate = false;
    SECTION( "ordinary terminal close flushes when its last scope pops" ) {}
    SECTION( "explicit immediate terminal close under a withheld owner" ) {
        immediate = true;
    }
    semantic_surface_manager manager( "terminal-owner-run" );
    std::vector<semantic_action_receipt> receipts;
    manager.set_receipt_observer( [&receipts]( const semantic_action_receipt &receipt ) {
        receipts.push_back( receipt );
    } );
    int effects = 0;
    std::optional<semantic_surface_scope> menu;
    menu.emplace( manager, "main_menu", "Main menu", std::map<std::string, std::string> {},
    std::vector<semantic_action_descriptor> { { "main_menu.quit", "", "Quit", true } },
    [&effects, &immediate]( const semantic_action_request & ) {
        ++effects;
        return semantic_action_dispatch_result{ true, "", "", false, !immediate };
    } );
    if( immediate ) {
        REQUIRE( manager.withhold_parent_authority_until_recreated( menu->surface_id() ) );
    }
    const auto frame = *manager.top();
    const semantic_action_request quit { manager.run_id(), frame.surface_id, frame.frame_id,
                                        "quit", "main_menu.quit", std::nullopt, {} };
    REQUIRE( manager.submit_request( quit ) );
    REQUIRE( menu->consume_request() );
    CHECK( receipts.size() == ( immediate ? 1 : 0 ) );
    menu.reset();
    CHECK_FALSE( manager.top() );
    REQUIRE( receipts.size() == 1 );
    CHECK( receipts.front().accepted );
    CHECK( receipts.front().resulting_frame_id.empty() );
    semantic_surface_scope later_world( manager, "world", "World" );
    CHECK( receipts.size() == 1 );
    CHECK_FALSE( manager.submit_request( quit ) );
    REQUIRE( receipts.size() == 2 );
    CHECK( receipts.back().resulting_frame_id.empty() );
    CHECK_FALSE( later_world.consume_request() );
    CHECK( effects == 1 );
}
