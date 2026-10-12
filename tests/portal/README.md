# portal tests

Everything here is fictional (vehicle "demo", MACs `02:00:00:00:00:xx`). Contract:
`docs/portal-spec.md`. Run: `.venv/bin/python -m pytest tests/portal -q`, `make -C tests/host`,
`.venv/bin/esphome config tests/build/portal/test*.yaml`.

<!--
COVERAGE  PS ID -> test (a "-" is a gap, "bench" means the PR carries the bench record)

PS-G-01      script/check.sh run as a whole (no single test)
PS-G-02      test_asset::test_our_own_files_carry_nothing_private, test_envelope_parity::test_fixtures_are_fictional
PS-G-03      test_asset::test_every_portal_build_yaml_is_in_the_ci_compile_matrix
PS-M0-01     test_asset::test_spec_exists_and_is_neutral
PS-M1-01     test_asset::test_component_layout_exists, test_portal_core_includes_only_the_freestanding_headers, test_portal_core_allocates_nothing
PS-M1-02     test_schema.py (all non-actions cases)
PS-M1-03     test_config_full::owns section
PS-M1-04     test_registry.py (in-process + codegen thunks), test_schema::test_series_unknown_provider_is_rejected_naming_it
PS-M1-05     test_schema::test_root_label_*, test_codegen::test_root_label_reaches_the_firmware, host test_portal_core (car root)
PS-M1-06     test_schema (position/size ranges), test_config_full::submodule section
PS-M1-07     host test_portal_core.cpp (ported core cases), test_envelope_parity::test_large_scenario_*
PS-M1-08     test_envelope_parity (byte-identical golden, schema), test_wire_schema::test_golden_fixtures_validate
PS-M1-09     test_wire_schema.py (RULES table, never-raises, CLI)
PS-M1-10     test_codegen::test_web_asset_*, test_default_asset_*, test_config_full web_asset, test_schema asset keys
PS-M1-11     test_asset::test_page_*
PS-M1-12     tests/build/portal/test-minimal (cfg here, cc in CI)
PS-M2-01     test_asset::test_no_own_httpd_*, test_component_registers_via_web_server_base, test_codegen::test_web_server_base_is_loaded_*
PS-M2-02     test_config_full::test_portal_with_web_server_is_rejected
PS-M2-03     test_codegen::test_port_reaches_web_server_base, test_config_full::..._captive_portal_and_ap
PS-M2-04     test_schema auth tests, test_codegen auth defines / warning, test_config_full actions-without-auth
PS-M2-05     host test_portal_actions::actions_ps_m2_05_* (inactive + AP-active matcher)
PS-M2-06..10 bench (M5)
PS-M2-11     bench (numbers on the team board / PR)
PS-M2-12     host actions_ps_m2_12_* (matcher); bench curl for the 302s
PS-M2-13     test_wire_schema links tests, host actions_ps_m2_13_*
PS-M3-01     test_schema actions tests, test_config_full actions
PS-M3-02     host actions_ps_m3_02_*, test_codegen::test_actions_storage_follows_declared_actions
PS-M3-03     host actions_ps_m3_03_*, test_wire_schema ACTION_RULES, test_envelope_parity actions
PS-M3-04     host actions_ps_m3_04_*
PS-M3-05     host actions_ps_m3_05_*, bench for ?after=
PS-M3-06/07  bench
PS-M3-08     test_asset::test_page_renders_each_control, test_page_sends_json_via_fetch_post, test_page_script_parses_with_node
PS-M3-07/v1.3 rules  test_handler_rules.py (503 on lock timeout, shutdown B1, body reset N1: grep-level); 411/form>1KiB/503 behaviour: bench (BENCH.md)
PS-M3-09     test_asset::test_entity_calls_are_not_made_from_the_request_handler (grep level; review by the spec owner)
PS-M4-01     tests/build/portal/test.esp32-c6-idf.yaml, test_asset::test_build_yamls_transmit_on_no_bus
PS-M4-02     tests/build/portal/test-asset.esp32-c6-idf.yaml
PS-M4-03     review
PS-M5-*      bench
-->

| file | tier |
|---|---|
| `test_schema.py` | py: YAML schema accept/reject |
| `test_config_full.py` | py: assembled-config rules via the ESPHome loader |
| `test_registry.py`, `fixtures/ext/` | py: provider registry + codegen thunks |
| `test_codegen.py` | py: `esphome compile --only-generate` (defines, port, asset bytes, owns) |
| `test_wire_schema.py`, `wire.py` | py: `script/portal_schema.py` rule table |
| `test_envelope_parity.py`, `fixtures/envelopes/` | py + host emitter (`tests/host/emit_portal_envelope.cpp`) |
| `test_handler_rules.py` | py: grep-level handler rules (shutdown, body reset, 503) |
| `BENCH.md` | bench checklist with exact curl commands |
| `test_asset.py` | py: component sources, built-in page, privacy |
| `tests/host/test_portal_core.cpp`, `test_portal_actions.cpp` | host C++ (ASan/UBSan) |
| `tests/build/portal/` | cfg + cc |
