Every knob behaves the same way, and so does every slider; this text comes from the same source as the plugin's Help panel. A knob has two parts: spin, click or flick the ring, or drag the centre up and down.

## Adjusting

| Action | macOS | Windows |
|---|---|---|
| Spin the ring | Turn around the knob's outer ring and the value follows the angle; drag into the gap at the bottom and it stays at the end you came from instead of wrapping from maximum to 0 | Same |
| Click the ring | Jumps straight to where you clicked | Same |
| Flick | Flick the ring and let go, and it keeps gliding a little before it settles; pause before letting go and it doesn't | Same |
| Drag the centre | Hold the centre of a knob and drag up or down (left / right works too); drag a slider left or right | Same |
| Fine adjustment (8× slower) | Hold ⌘ while dragging or scrolling; on the ring, holding ⌘ switches to relative turning: pressing doesn't jump, the value moves slowly from where it is, and pressing ⌘ halfway through a drag doesn't jump either | Hold Ctrl while dragging or scrolling; on the ring, holding Ctrl switches to relative turning in the same way |
| Back to the default | Double-click | Double-click |
| Type a value | Option-click: Return confirms, Esc cancels, clicking elsewhere also confirms; full-width digits from a Chinese input method are accepted | Alt-click (releasing Alt won't activate the host's menu bar) |
| Small steps | Mouse wheel | Mouse wheel |

As a knob turns, a cyan comet tail trails behind its pointer, longer and thicker the faster it moves. When the value changes from a click on the ring, a double-click back to the default, host automation, a preset or undo, the pointer springs over to it and settles exactly on the parameter value. While you drag, a dark value bubble appears above the control (with a small “Fine” tag during fine adjustment). Every parameter goes through the host's parameter attachment: one drag = one gesture (the extra glide after a flick is part of it), and automation and undo are recorded per gesture.

## Colours

Cyan = your settings; violet = GLAZE's automatic increment right now; orange = needs attention (true peak over the ceiling, guard relief “Limited · …”, invalid (NaN / Inf) audio blocks caught…).

## Window

- A fixed 1200 × 820 canvas that scales proportionally from 70% to 200%. Settings offers five common sizes + a slider + a preview in proportion to your screen (with a warning if it won't fit); you can also drag the bottom-right corner, which keeps the aspect ratio.
- The scale is saved with the project; the default size for new windows is set in Settings and follows you.

## Settings and Help

- **Language**: switch between `中` and `EN` in one click; every instance switches at once.
- **Reduce motion**: turns off particles, ripples and crack fade-ins; knobs show no comet tail and no spring, and a flick doesn't glide; the dot in the top bar's status pill doesn't pulse either.
- **Help**: the original text of the control hints above.

## Hover help

Appears after a 600 ms hover, in three lines: name · what it does / what you'll hear · where to start. For CLAY's three lamps, the second line is the live value (“Low cut 22 Hz · No mono · High width +0.8 dB”) and the third is this song's cost note or an old-project hint — or, while a House genre's out-of-phase hint is lit, advice to narrow the lows or align their phase in the mix.
