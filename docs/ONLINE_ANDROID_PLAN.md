# Online multiplayer and Android support

## User-facing requirements

- Preserve the existing Windows/desktop launcher and local multiplayer.
- Add an Online mode with **Host session** and **Join session** flows.
- The host receives a short, human-readable session code; other players enter it to join.
- Each remote player can choose a display name and a custom color. Replace the current limited color selection UI with a hue/saturation color wheel and a brightness/value control, with a live preview.
- Add in-game text chat. The chat overlay must be collapsible/minimizable and must not capture game controls while minimized.
- Add Android build support with optional on-screen touch controls. Touch controls can be toggled off for controller use.
- Keep the current SML2 local co-op and all existing single-player behavior intact.

## Architecture decisions to validate before implementation

### Online transport

A session code is an identifier, not a network connection. Online play needs a rendezvous/session service and a transport that works across NATs and restrictive networks. Prefer a small relay-backed service for the first release rather than requiring players to configure port forwarding. The native client should communicate with the service through a narrow interface so the service can be changed without rewriting game logic.

Do not assume deterministic lockstep works for every supported ROM/recompilation mode. First audit how the current runtime advances frames and captures input, then select a synchronization approach and explicitly handle latency, disconnects, host departure, and version mismatch. Never claim that online play works until two separate clients have completed an end-to-end test.

### Player identity and colors

Keep online identity separate from the existing local player slots and saved local controller bindings. Validate and length-limit player names, use a stable per-session player ID, and send color as RGB/HSV metadata rather than as game assets. A color wheel should be a real interactive control, not a static image, and should support mouse, touch, and keyboard/controller navigation where practical.

### Chat

Chat messages should be bounded in length and rate, sanitized before drawing, and tagged with the session player ID/name/color. The panel should have expanded and minimized states, a visible unread indicator while minimized, and a way to focus the text input without accidentally controlling the game.

### Android and touch controls

The project currently uses SDL2, so investigate SDL's Android toolchain and lifecycle before introducing platform-specific code. Add a dedicated touch input layer that feeds the existing button/d-pad abstraction. Provide a settings toggle for touch controls and ensure physical controllers remain usable. Test pause/resume, orientation/resizing, audio lifecycle, storage paths, and APK packaging. Keep platform code isolated from emulator/game logic.

## Implementation sequence

1. **Audit and foundations:** map current input, UI, player identity/color, save/settings, and frame loop APIs. Add platform-neutral interfaces and tests without changing working gameplay.
2. **Color wheel:** implement a reusable UI widget and replace the limited color chooser; verify mouse interaction and preserve saved settings.
3. **Touch input:** add SDL finger events and an overlay that maps to the existing input state; add a persisted enable/disable setting.
4. **Online session shell:** add Host/Join screens, session-code UX, connection status, and player roster using a test transport before synchronizing gameplay.
5. **Gameplay synchronization:** implement and test the selected transport/sync model, disconnect handling, and host/client lifecycle.
6. **Chat:** add send/receive, minimized overlay, unread count, and input focus behavior.
7. **Android packaging and end-to-end QA:** add Android build configuration and test the APK on an emulator and a physical device; test two distinct clients online.

## Acceptance checklist

- Existing desktop single-player and local multiplayer still work.
- Two independent clients can host/join using a code and see each other's connection state.
- Names and colors are per-player and persist for the intended scope.
- Color wheel works with mouse and touch.
- Chat sends/receives, minimizes, restores, and does not eat gameplay input while minimized.
- Android touch controls work by default (or according to the agreed default) and can be disabled without breaking a controller.
- Build/test status and any remaining end-to-end limitations are reported honestly.

## Current status

Planning document only. No online transport, chat, color-wheel widget, or Android APK support is implemented by this document. Implementation must be committed in small, testable steps on the feature branch.
