#ifndef GUARD_CONFIG_TELEMETRY_H
#define GUARD_CONFIG_TELEMETRY_H

// Telemetry for the LLM fight tracker (tools/llm_tracker), a development tool.
// When TRUE, the game writes battle and leveling events to an in-RAM ring buffer that the
// tracker's mGBA script reads. Keep it FALSE for releases: every call compiles away and the
// buffer takes no memory.
#define TELEMETRY_ENABLED   FALSE

#endif // GUARD_CONFIG_TELEMETRY_H
