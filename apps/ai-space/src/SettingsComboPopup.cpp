#include "SettingsComboPopup.h"
#include "LiquidPopup.h"
namespace SettingsComboPopup {
void install(QWidget *form) { LiquidPopup::installComboPopups(form); }
}
