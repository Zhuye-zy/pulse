# Issues #86 / #88 verification

## #86: elevated context-menu commands

The static registry command path previously used `CreateProcessW` and discarded its failure. HiBit Uninstaller 4.0.10 declares `requireAdministrator`; a non-elevated caller therefore cannot launch it through that path.

Evidence from the official portable package:

- Download: https://www.hibitsoft.ir/HiBitUninstaller/HiBitUninstaller-Portable-4.0.10.zip
- ZIP SHA-256: `986B1E07DE8037CDA70E4D2CF01010E2660042F79F233BBAC91846DE94741646`.
- Executable version: `4.0.10.0`; embedded manifest requests `requireAdministrator`.
- `pulse_shell_command_test --probe-elevation <HiBit executable>` returned `created=0 win32_error=740`. The probe requests a suspended child and terminates it if unexpectedly created; HiBit was not executed or installed.

The fix retains ordinary process launching. Only error 740 delegates once to Windows `ShellExecuteExW` with `runas`, preserving the executable, argument tail, working directory and owner. UAC cancellation is quiet; other failures are reported. The runtime event records both error codes and elapsed time without logging command arguments or paths.

`pulse_shell_command_test`: 10 checks passed, including error 740, cancellation, failed elevation, quoted Unicode paths, unquoted paths containing spaces, and a real ordinary child receiving its arguments and working directory. The elevation API itself is substituted in deterministic tests: this is not a claim that HiBit's full monitor-install workflow was exercised on the reporter's Windows 10 machine.

## #88: symbolic links and junctions

Enumeration, worker metadata, notification patches and network snapshots retain the reparse tag. Only symbolic-link and mount-point tags receive the link type/arrow; cloud placeholders and unknown tags are not labeled as links. The UI shows distinct symbolic-link/junction types and overlays in lists, icons, column view and the details panel. No general layout or theme controls were moved.

Network snapshot v2 stores tags; v1 remains readable without inventing a link classification. Malformed/truncated records are rejected.

`pulse_reparse_entry_test`: 19 checks passed with elevation, including real junctions, directory and file symbolic links through both native and Win32 enumeration, cloud/unknown classification, labels and cache compatibility. The non-elevated run passed 15 checks and explicitly skipped real symbolic-link creation (1314); the elevated run covers those cases.

Build: affected `pulse`, `pulse_shell_command_test`, `pulse_reparse_entry_test` and `pulse_ops_test` targets compiled successfully. Only the two issue-specific test executables were run. Both are included in the existing release CI test list.

Visual verification used real link fixtures in an isolated test instance. Light details view at 100%, dark large icons at 125%, the column strip, and the junction/file-link details panel were inspected. A white-on-white dark-theme overlay found in the first capture was fixed and recaptured; the overlay restores shared brush colors after drawing. File-link text preview also remained functional. Final local captures: `build/issue88-light-details.png` and `build/issue88-dark-large-icons.png`. Existing layout widths, toolbar controls and type-sort semantics are unchanged.

The target-display follow-up below supersedes the original overlay. The exact HiBit/Windows 10 end-to-end workflow still needs confirmation on that environment.

### Overlay visual refinement

Replaced the oversized diagonal arrow with a cached, filled curved shortcut arrow. The badge is capped at 18 DIP, uses a fine rounded border and subdued blue-gray colors, and tucks into the transparent padding of large Shell icons. Small list icons retain their corner alignment. High-contrast mode uses system theme foreground/background; renderer resets discard the cached geometry.

Rebuilt the affected `pulse` target and visually checked light details (100%), dark small/large icons (125%) and light large icons (200%). Captures are in `build/link-overlay-{light-list,dark-small,dark-large,light-large-200}.png`. No link classification or layout logic changed in this refinement.

### D concept: target pill and artwork alignment

The chosen D concept replaces the intermediate overlay above. Links use an accent-colored circular arrow at rest; hover or selection expands it into a target-name pill in icon views and a target-path pill after the name in list/details views. Expansion stays within the item's available width, does not add a line, and settles without keeping the frame pump active. Rename hides the expanded label. Very narrow rows retain the icon badge when there is no room for a readable target label.

The worker reads stored `.lnk` targets, local `.url` values, and one-hop symlink/junction destinations without fetching URLs or following reparse chains. Display destinations are independent of shortcut penetration metadata, including broken targets. Link add/modify/rename notifications request a full worker refresh; batch preflight preserves the old snapshot until that refresh.

Both Shell icons and decoded thumbnails retain alpha bounds. Icon grids align the visible artwork with the existing name baseline and put the badge inside that artwork. Normal previews keep their centered fit. Thumbnail resize/failure fallbacks share the same alignment.

Targeted validation: `pulse_link_destination_test` passed 73 checks with elevation (including real symlinks and single/batch notification cases); `pulse_link_pill_test` passed 10 checks. Native input automation reported `failed to activate captured window`, so staged renderer captures do not establish end-to-end mouse/keyboard behavior.

Final build of `pulse` and `pulse_shell_icons_test` succeeded. `pulse_shell_icons_test` passed 39 checks, including 13 alpha-bound/thumbnail-layout checks and real Direct2D icon conversion/cache recreation. Six staged captures were inspected: dark large selected/closed, dark medium hover, light details selected, light large selected, and light narrow medium hover (`build/link-pill-*.png`). Selection staging now applies visibility preferences before selecting, matching the normal view-model initialization order. The captures confirm expansion, closing, row/cell clipping, and thumbnail-loaded bottom alignment; live input automation remains unverified as noted above.

An unrelated concurrent UI-font-size change hit MSVC C1061 during this build. Its new input branch was folded into the existing shared settings switch, preserving that setting's behavior and avoiding another nested else-if. No settings control was moved.
