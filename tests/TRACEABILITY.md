# genet.device: traceability

Written by `harness/trace.py` (`run.sh host` rewrites it): every item the suite must cover, with the tests that cover it.

**Uncovered: 0** of 70 items.

## netdev commands

| Item | Tests |
|---|---|
| NETDEV_CMD_ATTACH | `hw/test_smoke.py::test_auto_brings_genet_up_as_netdev` |
| NETDEV_CMD_DETACH | `hw/test_cycles.py::test_the_interface_comes_back_after_every_remove` |
| NETDEV_CMD_START | `logic/test_coalesce.py::test_a_profile_set_while_stopped_is_programmed_at_start`, `logic/test_coalesce.py::test_a_stopped_unit_stores_the_values_and_start_programs_them`, `logic/test_netdev.py::test_a_stopped_unit_stores_the_filter_and_a_running_one_programs_it` |
| NETDEV_CMD_STOP | `hw/test_cycles.py::test_the_interface_comes_back_after_every_remove` |
| NETDEV_CMD_SET_RXFILTER | `logic/test_netdev.py::test_a_filter_needs_an_attached_stack_and_the_list_it_counts`, `logic/test_netdev.py::test_a_stopped_unit_stores_the_filter_and_a_running_one_programs_it`, `logic/test_netdev.py::test_fifteen_groups_are_kept_exact_and_a_sixteenth_falls_back_to_promiscuous` and 1 more |
| NETDEV_CMD_SET_COALESCE | `hw/test_sana2.py::test_coalescing_set_beside_sana2_and_released`, `logic/test_coalesce.py::test_a_running_unit_takes_the_values_at_once`, `logic/test_coalesce.py::test_a_stopped_unit_stores_the_values_and_start_programs_them` and 5 more |
| NETDEV_CMD_SET_RX_PROFILE | `logic/test_coalesce.py::test_a_profile_needs_a_netdev_stack_and_a_known_profile`, `logic/test_coalesce.py::test_a_profile_set_while_stopped_is_programmed_at_start`, `logic/test_coalesce.py::test_each_profile_programs_its_moderation` and 2 more |
| NETDEV_CMD_GET_STATS | `hw/test_smoke.py::test_netdev_stats_counts_the_traffic_and_sees_the_link` |
| NETDEV_CMD_GET_COUNTERS | `hw/test_smoke.py::test_netdev_stats_counts_the_traffic_and_sees_the_link` |
| NETDEV_CMD_GET_LINK | `hw/test_link.py::test_the_link_comes_up_as_the_prefs_ask`, `hw/test_smoke.py::test_netdev_stats_counts_the_traffic_and_sees_the_link` |
| NETDEV_CMD_SET_MAC | `logic/test_netdev.py::test_the_owner_sets_the_station_address_before_start`, `logic/test_netdev.py::test_the_station_address_is_refused_to_others_and_short_requests` |

## SANA-II commands

| Item | Tests |
|---|---|
| S2_DEVICEQUERY | `hw/test_smoke.py::test_any_opener_s_queries_answer`, `logic/test_sana2.py::test_device_query_fills_the_whole_structure_when_there_is_room`, `logic/test_sana2.py::test_device_query_leaves_raw_mtu_out_of_a_30_byte_structure` and 1 more |
| S2_GETSTATIONADDRESS | `hw/test_smoke.py::test_any_opener_s_queries_answer` |
| S2_GETGLOBALSTATS | `hw/test_smoke.py::test_any_opener_s_queries_answer` |
| S2_GETEXTENDEDGLOBALSTATS | `hw/test_smoke.py::test_any_opener_s_queries_answer` |
| S2_GETSPECIALSTATS | `hw/test_smoke.py::test_any_opener_s_queries_answer` |
| S2_SAMPLE_THROUGHPUT | `hw/test_sana2.py::test_every_kept_request_is_taken_back` |
| S2_ONEVENT | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_every_kept_request_is_taken_back`, `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach` and 1 more |
| S2_ADDMULTICASTADDRESS | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `logic/test_mcast.py::test_a_request_that_is_not_a_multicast_range_is_refused`, `logic/test_mcast.py::test_an_address_added_offline_is_kept_and_reaches_the_registers_only_online` and 2 more |
| S2_ADDMULTICASTADDRESSES | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_the_multicast_filter_passes_what_was_asked_for`, `logic/test_mcast.py::test_a_range_takes_a_slot_per_address_in_order` and 2 more |
| S2_DELMULTICASTADDRESS | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `logic/test_mcast.py::test_a_request_that_is_not_a_multicast_range_is_refused`, `logic/test_mcast.py::test_deleting_what_was_never_added_is_no_error` and 1 more |
| S2_DELMULTICASTADDRESSES | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_the_multicast_filter_passes_what_was_asked_for`, `logic/test_mcast.py::test_a_range_is_taken_back_by_its_own_bounds_only` and 1 more |
| S2_CONFIGINTERFACE | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `logic/test_sana2.py::test_a_netdev_stack_holding_the_unit_refuses_the_claim`, `logic/test_sana2.py::test_configuring_reports_the_change_then_online_then_the_link` and 3 more |
| S2_ONLINE | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_offline_and_online_raise_their_events_and_the_stack_recovers`, `logic/test_sana2.py::test_a_netdev_stack_holding_the_unit_refuses_the_claim` and 3 more |
| S2_OFFLINE | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_offline_and_online_raise_their_events_and_the_stack_recovers`, `logic/test_sana2.py::test_going_offline_answers_the_reads_and_reports_offline_to_the_waiters` and 1 more |
| CMD_READ | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_every_kept_request_is_taken_back`, `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach` and 9 more |
| S2_READORPHAN | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_sana2.py::test_every_kept_request_is_taken_back`, `logic/test_sana2.py::test_a_declining_packet_filter_leaves_the_read_pending_and_the_orphan_reads_unfiltered` and 1 more |
| CMD_FLUSH | `logic/test_sana2.py::test_flush_answers_every_kept_request_with_aborted`, `logic/test_sana2_tx.py::test_a_close_or_a_flush_answers_the_waiting_writes_aborted` |
| CMD_WRITE | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach`, `logic/test_sana2_tx.py::test_a_full_ring_keeps_writes_waiting_in_order_until_the_hardware_has_sent` and 5 more |
| S2_BROADCAST | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach`, `logic/test_sana2_tx.py::test_broadcast_and_multicast_writes_go_to_their_addresses` |
| S2_MULTICAST | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach`, `logic/test_sana2_tx.py::test_broadcast_and_multicast_writes_go_to_their_addresses` |

## new-style commands

| Item | Tests |
|---|---|
| NSCMD_DEVICEQUERY | `hw/test_smoke.py::test_any_opener_s_queries_answer`, `hw/test_smoke.py::test_auto_brings_genet_up_as_netdev`, `logic/test_unittask.py::test_a_short_or_missing_result_is_refused` and 2 more |

## SANA-II events

| Item | Tests |
|---|---|
| S2EVENT_BUFF | `logic/test_sana2.py::test_a_copy_that_fails_or_is_missing_answers_the_read_with_a_buffer_error`, `logic/test_sana2_tx.py::test_a_failing_copy_drops_the_write_and_the_unit_task_reports_its_events` |
| S2EVENT_CONFIGCHANGED | `logic/test_sana2.py::test_configuring_reports_the_change_then_online_then_the_link` |
| S2EVENT_CONNECT | `logic/test_sana2.py::test_an_event_wait_ends_at_once_on_a_state_that_holds_and_later_on_a_change`, `logic/test_sana2.py::test_configuring_reports_the_change_then_online_then_the_link` |
| S2EVENT_DISCONNECT | `logic/test_sana2.py::test_an_event_wait_ends_at_once_on_a_state_that_holds_and_later_on_a_change` |
| S2EVENT_ERROR | `logic/test_mcast.py::test_no_memory_for_a_range_fails_the_add_and_reports_a_software_error`, `logic/test_sana2.py::test_a_copy_that_fails_or_is_missing_answers_the_read_with_a_buffer_error`, `logic/test_sana2_tx.py::test_a_failing_copy_drops_the_write_and_the_unit_task_reports_its_events` |
| S2EVENT_OFFLINE | `hw/test_sana2.py::test_offline_and_online_raise_their_events_and_the_stack_recovers`, `logic/test_sana2.py::test_going_offline_answers_the_reads_and_reports_offline_to_the_waiters` |
| S2EVENT_ONLINE | `hw/test_sana2.py::test_offline_and_online_raise_their_events_and_the_stack_recovers`, `logic/test_sana2.py::test_configuring_reports_the_change_then_online_then_the_link` |
| S2EVENT_RX | `logic/test_sana2.py::test_a_copy_that_fails_or_is_missing_answers_the_read_with_a_buffer_error` |
| S2EVENT_SOFTWARE | `logic/test_mcast.py::test_no_memory_for_a_range_fails_the_add_and_reports_a_software_error`, `logic/test_sana2.py::test_a_copy_that_fails_or_is_missing_answers_the_read_with_a_buffer_error`, `logic/test_sana2_tx.py::test_a_failing_copy_drops_the_write_and_the_unit_task_reports_its_events` |
| S2EVENT_TX | `logic/test_sana2_tx.py::test_a_failing_copy_drops_the_write_and_the_unit_task_reports_its_events` |

## prefs keys

| Item | Tests |
|---|---|
| prefs UNIT_TASK_PRIORITY | `logic/test_prefs.py::test_a_priority_that_is_not_a_number_keeps_the_default`, `logic/test_prefs.py::test_every_key_is_read`, `logic/test_prefs.py::test_the_unit_task_priority_is_clamped_to_6_to_19` |
| prefs LINK_MODE | `hw/test_link.py::test_the_link_comes_up_as_the_prefs_ask`, `logic/test_prefs.py::test_an_unknown_link_mode_keeps_auto`, `logic/test_prefs.py::test_autoneg_off_forces_the_link_mode_in_either_order` and 2 more |
| prefs AUTONEG | `hw/test_link.py::test_the_link_comes_up_as_the_prefs_ask`, `logic/test_prefs.py::test_a_switch_that_is_neither_keeps_its_default`, `logic/test_prefs.py::test_autoneg_off_at_1000fd_keeps_autonegotiation_advertising_1000fd_only` and 4 more |
| prefs FLOW_CONTROL | `hw/test_link.py::test_the_link_comes_up_as_the_prefs_ask`, `logic/test_prefs.py::test_a_switch_that_is_neither_keeps_its_default`, `logic/test_prefs.py::test_every_key_is_read` and 1 more |
| prefs USE_MIAMI_WORKAROUND | `hw/test_sana2.py::test_the_miami_workaround_rounds_every_copy_to_longwords`, `logic/test_prefs.py::test_a_switch_that_is_neither_keeps_its_default`, `logic/test_prefs.py::test_every_key_is_read` and 3 more |
| prefs RX_POOL_BUFS | `logic/test_prefs.py::test_an_explicit_rx_pool_is_clamped_to_512_to_4096_and_0_is_auto`, `logic/test_prefs.py::test_an_rx_pool_that_is_not_a_count_keeps_auto`, `logic/test_prefs.py::test_every_key_is_read` |
| prefs defaults | `logic/test_prefs.py::test_nothing_set_gives_the_defaults` |
| prefs grammar | `logic/test_prefs.py::test_a_later_line_overrides_an_earlier_one`, `logic/test_prefs.py::test_comments_and_lines_without_a_setting_are_skipped`, `logic/test_prefs.py::test_keys_and_values_are_read_in_any_case_with_blanks_and_crlf` |
| prefs 3.x keys | `logic/test_prefs.py::test_the_3x_keys_and_unknown_ones_are_ignored` |

## the claim

| Item | Tests |
|---|---|
| claim: nobody holds (opening holds nothing; the interface removed, either personality takes the unit) | `hw/test_claim.py::test_nobody_holds_the_unit_once_the_interface_is_removed` |
| claim: netdev holds (ATTACH to DETACH; SANA-II's claim, multicast and datapath answer S2ERR_OUTOFSERVICE) | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_smoke.py::test_sana2_cannot_take_the_unit_netdev_holds`, `logic/test_mcast.py::test_a_netdev_stack_holding_the_unit_refuses_multicast_and_keeps_its_filter` and 1 more |
| claim: SANA-II holds (an opener's S2_CONFIGINTERFACE or S2_ONLINE to its close; ATTACH answers NDERR_BUSY) | `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach`, `logic/test_netdev.py::test_the_station_address_is_refused_to_others_and_short_requests` |
| claim: SANA-II after netdev (SANA-II takes the unit once netdev let go) | `hw/test_claim.py::test_sana2_takes_the_unit_once_netdev_let_go` |
| claim: netdev after SANA-II (netdev takes the unit once the last claimer closed) | `hw/test_claim.py::test_netdev_takes_the_unit_once_sana2_let_go` |
| issued-opener list (a close takes back only an Opener the open handed out) | `hw/test_claim.py::test_a_close_carrying_a_foreign_pointer_or_null_leaves_the_unit_working`, `logic/test_sana2.py::test_a_close_answers_the_openers_requests_and_the_last_claimer_releases_the_unit`, `logic/test_sana2.py::test_a_close_with_a_foreign_or_no_cookie_is_left_alone` and 2 more |

## refusal codes

| Item | Tests |
|---|---|
| NDERR_BUSY | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_smoke.py::test_sana2_cannot_take_the_unit_netdev_holds`, `hw/test_smoke.py::test_sana2_holds_the_unit_and_netdev_cannot_attach` and 1 more |
| NDERR_NOTATTACHED | `logic/test_coalesce.py::test_a_profile_needs_a_netdev_stack_and_a_known_profile`, `logic/test_coalesce.py::test_setting_coalescing_needs_a_holder_of_either_personality`, `logic/test_netdev.py::test_a_filter_needs_an_attached_stack_and_the_list_it_counts` |
| NDERR_BADPARAMS | `logic/test_coalesce.py::test_a_profile_needs_a_netdev_stack_and_a_known_profile`, `logic/test_coalesce.py::test_values_past_the_limits_are_refused_and_change_nothing`, `logic/test_netdev.py::test_a_filter_needs_an_attached_stack_and_the_list_it_counts` and 1 more |
| NDERR_BADVERSION | `hw/test_claim.py::test_nobody_holds_the_unit_once_the_interface_is_removed` |
| S2ERR_OUTOFSERVICE | `hw/test_claim.py::test_every_sana2_command_that_would_take_the_unit_is_refused_beside_netdev`, `hw/test_smoke.py::test_sana2_cannot_take_the_unit_netdev_holds`, `logic/test_mcast.py::test_a_netdev_stack_holding_the_unit_refuses_multicast_and_keeps_its_filter` and 3 more |
| S2ERR_BAD_STATE | `logic/test_sana2.py::test_online_needs_a_configured_unit_and_configuring_happens_once` |
| S2ERR_BAD_ARGUMENT | `logic/test_sana2.py::test_a_read_needs_an_opener_and_an_online_unit`, `logic/test_sana2.py::test_device_query_refuses_a_short_or_missing_structure`, `logic/test_sana2_tx.py::test_a_write_that_can_never_be_sent_is_refused_before_the_ring` |
| S2ERR_BAD_ADDRESS | `logic/test_mcast.py::test_a_request_that_is_not_a_multicast_range_is_refused` |
| S2ERR_NO_RESOURCES | `hw/test_sana2.py::test_every_kept_request_is_taken_back`, `logic/test_mcast.py::test_no_memory_for_a_range_fails_the_add_and_reports_a_software_error`, `logic/test_sana2.py::test_a_copy_that_fails_or_is_missing_answers_the_read_with_a_buffer_error` and 2 more |
| IOERR_OPENFAIL | `hw/test_claim.py::test_the_opens_the_driver_takes_and_refuses` |
| IOERR_UNITBUSY | `hw/test_claim.py::test_the_opens_the_driver_takes_and_refuses` |
| IOERR_BADLENGTH | `logic/test_unittask.py::test_a_short_or_missing_result_is_refused` |
| IOERR_ABORTED | `logic/test_sana2.py::test_a_close_answers_the_openers_requests_and_the_last_claimer_releases_the_unit`, `logic/test_sana2.py::test_a_declining_packet_filter_leaves_the_read_pending_and_the_orphan_reads_unfiltered`, `logic/test_sana2.py::test_an_aborted_read_is_answered_once` and 4 more |
