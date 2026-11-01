## Bugs

## New Features

- Add auxiliary modulation sources, like Phase Plant and Serum

- Add an automation matrix that lists all active automation from LFOs, Env, Macros, Note, Modwheel and allows changing their parameters
    - Extra navigation entry next to Routing/Matrix/FX called "Mod matrix"
    - Also has an extra "amount" slider per automation that fine-controls the modulation amount
    - Also shows the aux source
    - Allow using a automation source (LFOs, Env, Macros, Note, Modwheel) as an "aux source" for an existing automation by dragging it to a control's automation blip
    - The aux source controls the amount of the original automation being applied
    - Only one aux source allowed per automation

- Toggleable pitch tracking for operators, fixed frequncy if turned off

## Issues 

- When assigning too many automation sources to a control, the automation blips spill over onto other elements. Not sure how to solve this.


## Improvements 

- Move factory presets out of the C++ code, into committed .FMV files. They should be compiled into the code on build.
- In the LFO card, the rate knob area is a bit cramped

- The shimmer reverb does not sound good

## Architecture

- Linux support
- VST3 version?

## Organziational

- Decide on names for the major UI elements to put into documentation
