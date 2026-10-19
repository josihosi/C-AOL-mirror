#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coords_fwd.h"
#include "point.h"

class npc;
class item;

namespace llm_intent
{
// Offered labels and native pickup matching must use the same normalization.
std::string normalize_item_label( std::string_view text );
void enqueue_request( npc &listener, const std::string &player_utterance );
void enqueue_request( const npc &listener, const std::string &player_utterance );
void enqueue_ambient_request( npc &listener, const std::string &player_utterance );
void enqueue_requests( const std::vector<npc *> &listeners,
                       const std::string &player_utterance );
void prewarm();
void process_responses();
void process_response_for_test( npc &listener, const std::string &request_id,
                                const std::string &player_utterance,
                                const std::string &response_text );
void process_inventory_response_for_test( npc &listener, const std::string &response_text );
// Exact tree identity and quantities, excluding transient rot/temperature state.
std::string gift_tree_identity( const item &root );
void prepare_inventory_request_for_test( npc &listener, const std::string &request_id );
void process_prepared_inventory_response_for_test( npc &listener, const std::string &request_id,
        const std::string &response_text );
std::string build_inventory_prompt_for_test( npc &listener, const std::string &utterance );
std::string build_look_around_prompt_for_test( npc &listener, const std::string &utterance );
bool has_request_state_for_test( const npc &listener, const std::string &request_id );
void enqueue_random_requests();
void log_event( const std::string &message );
std::string classify_response_error_for_test( const std::string &error );
std::string sanitize_response_error_for_test( const std::string &error );
bool should_retry_response_for_test( const std::string &error );

std::string build_snapshot_for_test( npc &listener, const std::string &player_utterance,
                                     const std::string &request_id );
std::string build_action_prompt_for_test( const std::string &npc_name,
        const std::string &player_utterance,
        const std::string &snapshot );
std::string request_json_for_test();
size_t look_around_selection_limit_for_test();
std::vector<std::string> parse_look_around_response_for_test( const std::string &text,
        const std::vector<std::string> &allowed_names );
bool parse_move_field_for_test( const std::string &field, point &delta,
                                std::string &terminal_state,
                                std::string &error );
bool parse_action_csv_for_test( const std::string &csv, std::vector<std::string> &actions,
                                std::string &attack_target, std::optional<point> &move_delta,
                                std::string &move_terminal_state, std::string &error );
std::string parse_action_csv_speech_for_test( const std::string &csv );
std::string normalize_csv_separators_for_test( const std::string &csv );
std::string prepare_event_log_payload_for_test( const std::string &payload );
tripoint_abs_ms resolve_move_target_for_test( const tripoint_abs_ms &origin,
        const point &delta );
} // namespace llm_intent
