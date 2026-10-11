#pragma once

#include <windows.h>

// Dark popup menus: owner-drawn Win32 menus, so they keep the system's keyboard handling and accessibility.

struct MenuItem {
    UINT cmd;                 // 0 for a separator
    const wchar_t* label;
    wchar_t key;              // the letter that picks it while the menu is open
    const wchar_t* shortcut;  // shown right of the labels; may be empty
    bool enabled = true;
    bool checked = false;
};

// Shows the items in a popup below anchor (in screen pixels), owned by owner. Returns the chosen command, or 0.
UINT ShowPopupMenu(HWND owner, const RECT& anchor, const MenuItem* items, int count);

// Handles the owner-draw messages the owner gets while a popup shows. Returns whether it did, with the result.
bool HandlePopupMenuMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
