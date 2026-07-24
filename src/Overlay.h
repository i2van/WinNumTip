#pragma once

struct IUIAutomation;

// The numbered-badge overlay shown alongside the taskbar while the Windows key is
// held. A fresh layered window is created on Show and destroyed on Hide.
namespace Overlay {

// Register the overlay window class and initialize buffered paint. Call once.
void Init(HINSTANCE inst);

// Release resources acquired by Init (and destroy the window if still shown).
void Shutdown();

// Create and draw the overlay for the current taskbar state (no-op if already shown).
void Show(IUIAutomation* uia, HINSTANCE inst);

// Destroy the overlay window (no-op if not shown).
void Hide();

} // namespace Overlay
