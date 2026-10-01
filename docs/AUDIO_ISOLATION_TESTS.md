# LivePlay+ audio isolation tests

`server/tests/e2e/library-audio-isolation-e2e.js` starts the Release server with an isolated Plus directory, plays a looping local WAV, submits 1,000 unique server-path imports, then polls engine and import state. Each import performs SHA-256, metadata extraction, copy, durable journal writes and permitted compaction. It records the engine's underrun/discontinuity counters, callback timing, queue depth, process CPU and working-set metrics in `docs/AUDIO_ISOLATION_RESULTS.json`.

Acceptance criterion: **0 audible dropouts during library operations**. The automated proxy requires `underruns == 0`, `discontinuities == 0`, `callback_overruns == 0`, a real active output device, at least one rendered block, and maximum render-block time below the configured block budget. This verifies the engine's instrumented continuity; it does not replace listening/loopback capture on every Windows driver or hardware path.

Phase 1.6 Release Windows result (2026-10-01): 1,000 tracks in 23,572 ms; 0 underruns, 0 discontinuities, 0 callback overruns; max/average render block 2,088/415.08 μs with a 5,333.33 μs budget; minimum queued audio 26.67 ms; CPU 21,437 ms; working set 26,050,560 bytes and peak 30,285,824 bytes; one output device. The test passed.

The test is intentionally local: it does not depend on Internet, LLM, analysis models or a second audio engine. Before distributing a performance claim, repeat it on supported WASAPI/ASIO devices with antivirus enabled and capture an analog or digital loopback to verify perceived continuity.
