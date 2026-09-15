#!/bin/bash
# test-context-notes.sh — what the manual-test image says when it starts.
#
# Run by the image's default CMD (docker/Dockerfile.test-context) before it
# hands over a shell. It is deliberately a script rather than a long CMD
# string: this text is documentation, and documentation belongs somewhere it
# can be read, diffed and edited without touching the layer that builds the
# tree.
#
# The build tree is at /src/build, the sources at /src. Nothing here configures
# anything: the image was built in the reference execution context, so the
# binaries already find their plugins (<exe>/plugins/llm) and their tool
# declarations (the path the build baked in).

cat <<'EOF'
================================================================================
tools stack — manual test image
================================================================================
Built from the portable toolchain base: AlmaLinux 9, GCC 14.3.0, Boost 1.91.0.
Sources:   /src
Build:     /src/build   (Debug; bin/, lib/, every test binary)

THE DEMO (tools/example/deepseek_chat.cpp)
  /src/build/bin/tools_deepseek_chat

  A DeepSeek chat whose tools are the six intrinsic process tools, reached
  through ToolRegistry + ProcessToolSet + ProcessSessionStore. The model can
  ask for real programs on this machine; spawn_process, write_process_input
  and kill_process are confirmed at the terminal before they run.

  The system prompt is a persona plus the SET'S SKILL: the guidance from
  tools/intrinsic/toolsets/process/schemas/skill.yaml, injected as the
  skill.process section (ToolRegistry::inject_skills). That is what tells the
  model to wait rather than poll, what a session costs, and what a denied
  confirmation means — read it with /skill or --skill.

  Requirements:  an API key, and a TTY for the confirmation prompts.

  Start it with a key in the environment so the first prompt is only the
  reasoning-effort one:

      DEEPSEEK_API_KEY=sk-... /src/build/bin/tools_deepseek_chat

  Flags:
      --yes          approve every confirmed call without asking
      --tools        print the tool catalogue and exit (no key, no child)
      --skill        print the guidance the model was given, in full, and
                     exit (also no key, no child)
      --list-models  the provider's live model list and balance, then exit
      --max-steps N  model exchanges one message may take (default 12; raise
                     it when driving an interactive child, which costs one
                     round trip per look)
      --help         usage

  In the REPL: /tools, /skill, /sessions, /help, empty line quits.

THINGS WORTH TRYING (each one exercises a different part of the chain)
  1. "run seq 1 5 and show me the output"
        -> one spawn_process call that finishes inside its window: exit code
           and the whole output in the same tool result.
  2. "start cat in the background, feed it hello, then read back what it
      printed"
        -> spawn (session id) -> write_process_input -> read_process_output,
           three turns that share one session through the store.
  3. "start sleep 600 in the background, then kill it"
        -> a session that outlives its window, and kill_process ending it.
  4. "what is running right now?"   ->  poll_processes
  5. /sessions                      ->  the host's own view of the same table
  6. Answer "n" to a confirmation   ->  the call is refused, and the refusal
      is a tool RESULT: the model reads "security check denied: ..." and can
      adapt, instead of the turn dying.
  7. Drive another agent: "spawn a second copy of this program with
      /src/build/bin/tools_deepseek_chat, then send it a task and tell me what
      it answers"
        -> the outer model bootstraps an inner one and drives it through
           write_process_input / read_process_output. Each round of
           "feed it, look at what it said" is one model exchange, so that kind
           of work runs past the default budget: raise it with
           --max-steps 40 (or answer "continue" when it says it stopped).

OFFLINE CHECKS (no API key, no network, no child process)
  /src/build/bin/tools_deepseek_chat --tools
      Prints the catalogue the registry hands the model, the set's skill and
      its section in a prompt, and exits non-zero if any of the six
      declarations failed to load, a name is not routable, or skill.yaml did
      not load and inject.

  /src/build/bin/tools_deepseek_chat --skill
      The guidance itself, exactly as the model receives it.

  ctest --test-dir /src/build -R tools_deepseek_chat_catalogue --output-on-failure
      The same thing, as the suite the build ran.

THE WHOLE SUITE
  ctest --test-dir /src/build --output-on-failure
      Note: process_destructive keys on /.dockerenv (its own rule, see
      process/test/CMakeLists.txt), and /.dockerenv is here — so re-running
      the suite in this container engages it, and it churns processes while it
      runs. The build already ran the suite; see the build log for whether it
      engaged there.

A STAGED RELEASE (already built; exercises the other schema location)
  /src/stage/bin/tools_deepseek_chat
      The same demo as installed by `cmake --install`: the tool declarations
      AND skill.yaml travel with it at /src/stage/bin/schemas/process, which is
      the "beside the executable" path a deployment uses — this binary needs no
      source tree at all. Re-stage after a rebuild with:

          cmake --install /src/build --prefix /src/stage

CLEANUP IS THE CONTAINER'S JOB
  Children the model starts are signalled on exit (terminate_all before the
  store is dropped). Run the demo with --rm and a killed container takes any
  stray child with it; anything started by hand in this shell is yours.
EOF
