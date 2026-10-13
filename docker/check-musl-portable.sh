#!/usr/bin/env bash
# Validate the CI archive on a glibc host without a musl installation. Test
# executables travel separately and are never part of the worker distribution.
set -euo pipefail
if (( $# != 1 )); then
    echo "usage: $0 ARTIFACT_DIRECTORY" >&2
    exit 2
fi
artifacts=$(cd -- "$1" && pwd)
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=$(<"$repo/VERSION")
name="simplex-worker-v${version}-linux-x86_64-musl"
(cd "$artifacts" && sha256sum -c "$name.tar.gz.sha256")
temporary=$(mktemp -d)
trap 'rm -r -- "$temporary"' EXIT
tar -xzf "$artifacts/$name.tar.gz" -C "$temporary"
stage="$temporary/$name"
unset LD_LIBRARY_PATH LD_PRELOAD SIMPLEX_LOOP_HOOK_SCHEMA_DIR
unset SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR
unset SIMPLEX_READING_SCHEMA_DIR SIMPLEX_EDITING_SCHEMA_DIR SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR
# Validate the unmodified archive first, including actual-worker lifecycle.
python3 "$repo/docker/test-musl-runtime.py" "$stage" "$repo" "$artifacts/musl-launch-probe"

# Then add only test executables to the throwaway installation. Explicit loader
# invocation exercises the same resource lookup as the packaged worker wrapper.
# /src and /build were the compile-time paths; neither may rescue this test.
test ! -e /src
test ! -e /build
tar -xzf "$artifacts/musl-test-support.tar.gz" -C "$stage/bin"
run_test() {
    local binary=$1
    shift
    "$stage/lib/ld-musl-x86_64.so.1" --library-path "$stage/lib" \
        "$stage/bin/$binary" --log_level=error "$@"
}
run_test test_tools --run_test=the_set_offers_five_routable_tools,every_tool_is_declared_by_its_own_yaml_file,the_set_carries_its_skill_and_hands_it_to_a_prompt,installed_terminal_runtime
run_test test_registry_e2e --run_test=tty_selection_reaches_confirmation_before_launch
run_test test_reading_tools --run_test=defaults_schema_skill_and_attributes_match,installed_declarations_take_precedence_when_present,all_read_modes_preserve_expected_selection,selected_yaml_configuration_controls_read_limits
# Loading configuration from the installed YAML must not require recompilation.
declaration="$stage/bin/schemas/reading/read_text.yaml"
grep -Fxq '  max_file_bytes: 16777216' "$declaration"
grep -Fxq '  max_output_bytes: 65536' "$declaration"
sed -i -e 's/^  max_file_bytes: 16777216$/  max_file_bytes: 64/' \
    -e 's/^  max_output_bytes: 65536$/  max_output_bytes: 7/' "$declaration"
grep -Fxq '  max_file_bytes: 64' "$declaration"
grep -Fxq '  max_output_bytes: 7' "$declaration"
run_test test_reading_tools --run_test=selected_yaml_configuration_controls_read_limits
run_test test_editing_tools --run_test=schema_registration_confirmation_and_aligned_preview,installed_declarations_take_precedence_when_present
run_test test_load_system_prompt --run_test=default_prompt_is_loaded_beside_the_executable
run_test test_hub_remote_call_tools --run_test=toolset_registers_plan_and_owns_settings,subagent_schemas_validation_and_attributes_agree
run_test test_tool_extensions --run_test=load_register_execute_and_pin_handle
run_test test_loop_extensions --run_test=load_register_publish_and_release
# This test was built with a deliberately nonexistent source fallback. Alter
# the installed configuration and require the hook to observe that exact value.
hook_config="$stage/bin/schemas/loop/context_statistic/config.yaml"
grep -Fq 'context_window_tokens: 128000' "$hook_config"
sed -i 's/context_window_tokens: 128000/context_window_tokens: 777/' "$hook_config"
"$stage/lib/ld-musl-x86_64.so.1" --library-path "$stage/lib" \
    "$stage/bin/test_loop_installed_config"
export SIMPLEX_QWEN_TEST_PLUGIN_DIR="$stage/bin/plugins/llm"
run_test test_qwen_plugin --run_test=options_defaults_and_atomic_updates
printf 'musl portable archive and installed resources passed\n'
