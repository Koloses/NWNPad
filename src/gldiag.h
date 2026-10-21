#pragma once
// OpenGL diagnostics ([Developer] GLDiag=1): see gldiag.cpp
void GLDiag_Init(const char* binDir);     // early, before the game creates its GL context
void GLDiag_PreOverlay();                 // SwapBuffers, before the overlay
void GLDiag_PostOverlay();                // SwapBuffers, after the overlay
void GLDiag_HookSwap();                   // when the gamepad features are off
bool GLDiag_Capturing();
