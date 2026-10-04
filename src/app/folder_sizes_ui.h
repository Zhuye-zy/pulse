#pragma once
namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }
void FillFolderSizes(AppState& state, ui::WindowViewModel& vm);
// Re-sorts Size-ordered listings whose folder totals changed (#58), at most
// every 750 ms per listing and never under a press, drag or rename. Returns
// true while a listing still waits for its turn.
bool ResortForFolderSizes(AppState& state);
}
