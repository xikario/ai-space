#pragma once
#include <QString>
struct MigrationResult { bool ok = true; bool knowledgePending = false; int copied = 0; QString error; };
// Preferences and UI snapshots are small enough to prepare before the window;
// a multi-gigabyte knowledge DB is imported after the window is usable.
MigrationResult migrateLegacyData(bool includeKnowledge = true);
