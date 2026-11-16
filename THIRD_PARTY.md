# Third-party components / Сторонние компоненты

The project itself is licensed under **GPL-3.0-or-later** (see `LICENSE`).
Сам проект распространяется по лицензии **GPL-3.0-or-later** (файл `LICENSE`).

## Included in this repository / Входят в репозиторий

| Component | Where | License |
|-----------|-------|---------|
| DejaVu Sans Mono (font) | `pkg/assets/fonts/DejaVuSansMono.ttf`, license text `pkg/assets/fonts/LICENSE-DejaVu.txt` | Bitstream Vera / DejaVu license (free, GPL-compatible) |
| stb_image v2.30 | `pkg/src/stb_image.h` | public domain or MIT (author's choice; see the header of the file) |
| SHA-256 (own implementation, standard constants) | `pkg/src/sha256.cpp` | GPL-3.0-or-later (part of this project) |
| Logo, app icon, button/tag icons, QR codes | `pkg/gfx/`, `pkg/assets/images/` | made for this project; GPL-3.0-or-later for the artwork unless a file says otherwise. The PlayStation-style button shapes are generic symbols drawn by `tools/make_button_icons.py` |

## Used to build, NOT included / Нужны для сборки, в репозиторий не входят
- **OpenOrbis PS4 Toolchain** (PKG app, `pkg/`) — separate project and license. The build copies its runtime modules into the package;
  they are not stored here.
- **ps4-payload-sdk** (service payload, `src/`) — separate project and license.
- **GoldHEN** — used on the console to load the service payload; not part of this project.

## Not included on purpose / Намеренно не включены
- **No games and no PKG files.** The store only installs files that the owner of the console puts on a drive.
- **No game catalog data.** Covers, screenshots, descriptions and titles come from third-party stores and belong to their owners.
  The repository ships the *tools* (`tools/fetch_catalog.py`, `tools/translate_catalog.py`, …) so that you can build a catalog
  for your own use; the result (`data/`) is ignored by git and must not be published or redistributed.
- Public lists of releases (for example the source list used by `tools/fetch_catalog.py`) are downloaded by the user's own run of the tool.

## Trademarks / Товарные знаки
PlayStation, PS4 and related names are trademarks of Sony Interactive Entertainment. This project is not affiliated with,
endorsed by or sponsored by Sony or any game publisher. Game titles shown in the app are used only to identify files the user already has.
