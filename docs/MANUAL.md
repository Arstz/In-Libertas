# In Libertas editor manual

Please read this guide in full before using the editor, this would answer many of your questions before you encounter them down the line, I tried to keep it as concise as possible. Keybinds provided are default, rebind them from the `Settings->Keybinds` if needed. 

## Main workflow

Start by creating a new project file via `File->New project...` or importing spc with `File->Import SPC...`. When creating new project the editor will ask to provide a valid audio file and a jacket (2048x2048 is preferred), if importing instead, place them in the same folder as the spc itself (if you are importing spc, the files should be already named and arranged as needed and editor will recognize them). After you have created a project you can save it with `File->Save project...` and it will be written on disk as a .10no file (remeber to save periodically), which you can then open later, a single instance of an editor can open one project file at the time. To export a project use `File->Export project...` and select CustomCharts folder in your game installation, make sure to have Melon Loader configured and hook dlls installed.

## GUI

The editor is arranged into resiable widgets with dividers, a toolbar on the left is the only exception. 

### Toolbar

Contains Place `1`, Select `F` and Move `V` tools, as well as a logarithmic volume slider. An active tool will influence how you interact with the Flat View widget.

### Flat View

This is where you edit, place and remove your hitObjects (Note, Hold, Zone, Flick). A waveform is available on the left with the actual conveyor grid scrolling upwards when playback is enabled `Space` on the right, a red playhead shows your current position on the conveyor. The grid is defined by divisors, you can increase/decrease them by pressing `Up/Down` arrowkeys, or `Mousewheel + Control`. To scroll the conveyor by 1 divider use `Mousewheel` or `Left/Right` arrowkeys. Adjust the zoom of the conveyor with `+/-` or `Mousewheel + Alt`. A titlebar also has a `Sky/Ground/Both` filter, also can be cycled with `Tab`, filter determines which hitObjects are visible and can be interacted with. Playback speed can be adjusted with `[` to increase and `]` to decrease.

### Events

This widget contains a list of your events in the chart, there are 3 types of events: Timing (BPM/time signature), SV (Speed variation) and Lane (Turn ON/OFF selected lane), any event can be added at playhead time with the buttons at the bottom. A valid chart requires at least 1 BPM event to be exported, these events influence how divisors are rendred and hitObjects snapped. SV events are multipliers of the conveyor speed, they can carry negative to allow the conveyor to scroll upwards instead, however the hitObjects placed in negative SV regions will approach the receptors from below, leaving almost no time for the player to react to them. Lane events are visual only and not mandatory, the game would not block any imput on them but will dim/restore the lanes during gameplay. `Click` to select an event, `double-click` to jump to its time offset, select multiple with `Control` or a time offset range with `Shift` held while clicking, press `Delete` to remove events selected. Copy `Control + C`/Paste `Control + V`/Cut `Control + X` operations work as you would expect but Paste is applied at playhead. 

### Properties

Widget will show editable properties of a currently selected hitObject or event. You do not really need to use it unless you need to fine-tune something or making a gimmick chart.

### Metadata

Here you can edit your metadata (obviously), most important in order: 
- Chart ID - this would be injected into the game DB, the game sorts by this field so it will affect where the chart is placed and which filter key it responds to in song select menu, pick something unique and make sure it does not collide with any existing ids or other chart ids already installed;
- Jacket - an image to be used as a jacket inside song select menu and during gameplay (preferred size square, ideally 2048x2048);
- Song/Artist - this would be used as the display metadata in the song selection screen as well as during gameplay;
- Difficulty - does not work quite like others, this isntead changes the active difficulty in the project (all projects are created with 4 difficulty slots already, only populated will be exported).

Also non-mandatory fields:
- Preview start/end - these timings tell the game when to start playing preview in song select menu, keep it long enough to account for fade in/out the game layers on top;
- Rating - the only difficulty specific filed, only defines a number to be displayed in the UI inside song select menu, up to 19 works fine, higher values are allowed but would break UI sligtly;
- Background - defines background to be used during gameplay, for now a selector within vanilla charts.
- Jacket designer - only displayed inside transition to gameplay animation
- Chart designer - .10no project only field

### 3D view

A non-editable preview on the chart ingame will be played here, an adjustable speed parameter is also present at the titlebar. The view is mosly on par with the game, the only exception is the negative SV sections, the objects will render past the receptor but ingame will be destroyed instantly instead. A small card is displaying current difficulty, jacket and metadata.

### Verification

Here you are able to see non-blocking messages that would protect you against making accidental errors like placing 2 notes at the same time, or covering a note with a hold on top, these are only tips and you can ignore them if you know what you are doing. You can navigate to the timestamp by `double-click` on the relevant line.

## Actually placing hitObjects

The main way to place hitObjects is to use the Place tool:
- `Left click` would place a note;
- `Right click` to remove it;
- `Left drag` vertically will create a hold, horizontally - a multi note, diagonally - a multi hold.
- To create a flick drag horizontally with `Shift` held
- For zones use a `Alt` modifier while dragging

## Editing existing hitObjects

Last placed hitObject will be selected by default, indicated by a outline for ground hitObjects and handles for sky hitObjects. A Place tool works with 1 hitObject at the time, a `left click` on unselected hitObject will select it, then it could be moved if by dragging its body, hold endTime can be modified by dragging its end. Sky hitObject can be moved by the body as well as adjusting their size by the handles. Flicks have 2 handles, defining their size and X position, Zones have 2 pairs instead for their start and end segments, also 2 more on the sides which control the easing per side, either sine-in, sine-out or linear. A zone can be snapped while moving to another one if they have approximately the same segment size, snapped zones will share a handles per segment. 

## Batch operations

Select multiple hitObjects with Select tool by dragging, common editing commands can be used on a selection: Copy/Paste/Cut and Flip Horizontally `Control + H`/ Vertically `Control + J`. Move tool only moves the current selection in both axis, never modifies it. All operations are also available from the `Edit->` menu.
