# Luna

Launch GameStream applications from your PC on the CoreELEC device directly from the Kodi menu,
without exiting to another shell.

Amlogic build for the CoreELEC-NO nightly version, based on [Luna](https://github.com/TheChoconut/luna)
by TheChoconut, with fixes in moonlight-embedded itself.

## Installation

Kodi: Settings → Add-ons → Install from zip file.

## First Launch

1. Open the add-on settings and enter the host address—or simply click
   "Pair," and the host will be detected automatically.
2. Enter the displayed PIN on your computer.
3. Select an audio device if you need surround sound.

You can leave the other settings as they are: the defaults are designed
to ensure the stream starts immediately.
It is recommended to use the latest version of Sunshine.


## Usage

Folder paths can be used for widgets or shortcuts

For example, for the Luna plugin:
plugin://script.luna/?content_type=game

For example, for a folder named Apps:
plugin://script.luna/?action=apps&folder=Apps

For example, for a folder named PS3:
plugin://script.luna/?action=apps&folder=PS3


## Credits

- Original addon — [TheChoconut](https://github.com/TheChoconut/luna).
- Logo and fan art — [Ben Bidrava](http://sooulart.com).
- Icons and UI refinements — Vortex
- Moonlight builds — Vortex
- [moonlight-embedded](https://github.com/moonlight-stream/moonlight-embedded)
