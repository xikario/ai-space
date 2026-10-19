#pragma once
#include <QString>
struct MigrationResult { bool ok = true; int copied = 0; QString error; };
MigrationResult migrateLegacyData();
