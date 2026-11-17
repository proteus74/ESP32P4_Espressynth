#pragma once

// Safe start: keeps the device reachable when the boot preset is the problem.
//
// A preset that starves the network of memory (or crashes the firmware while loading) would lock
// the user out: without the network no other preset can be selected. In a safe start the engine
// loads nothing - the active slot is a scratch file that does not exist yet - and no preset, not
// the unnamed slot and not the ".last" marker is touched. Selecting any preset ends it.
//
// A safe start happens when
//  - the previous boot could not reach the Wi-Fi co-processor (that boot restarts once by itself),
//  - two boots in a row did not get healthy (crash, watchdog, power cut before the end of boot),
//  - a finger rests on the touch screen while the device starts.
// "Healthy" = the boot preset finished loading and the network came up (or is known to be broken
// even without a preset, so a dead co-processor does not cause a restart on every boot).
namespace safestart
{
// Call once before the boot preset is resolved. activeName: the preset that would be loaded
// ("" = the unnamed slot).
void begin(bool touchHeld, const char *activeName);

bool active();
const char *reason();      // "network", "boot attempts", "touch"; "" when not active
const char *skippedName(); // the preset that was not loaded ("" = the unnamed slot)
const char *scratchPath(); // the active slot during a safe start

// Any deliberate preset selection ends the safe start.
void leave();

// The boot preset finished loading (successfully or not - the firmware survived it).
void markPresetLoaded();
// Result of the network bring-up. A first failure with a preset loaded restarts into a safe start.
void markNetwork(bool ok);
} // namespace safestart
