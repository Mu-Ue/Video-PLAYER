#pragma once

// Registers the player for the current user (no admin rights needed), so Windows lists it under
// Settings > Default apps and Open with for every video type it plays, then opens that settings page.
bool RegisterApp();

// Removes everything RegisterApp wrote.
void UnregisterApp();
