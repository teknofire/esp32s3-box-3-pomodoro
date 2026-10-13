# Contributing

Thanks for your interest in improving the ESP32-S3-BOX-3 Pomodoro timer! Bug reports, fixes and new features are all welcome.

## Reporting bugs and ideas

Open a GitHub issue and include:

- What you did, what you expected, and what happened
- Your esp32 board-package, LovyanGFX and WebSockets versions
- Serial output (115200 baud) if relevant
- For API or WebSocket problems: the request you sent and the response. **Remove your API token first.**

## Development setup

1. Follow the setup steps in the [README](README.md#setup) to install the toolchain and create your own `secrets.h`.
2. Build with the same board settings as the README:

   ```sh
   arduino-cli compile -b "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB" .
   ```

3. Add `--upload -p <port>` to flash a connected box.

## Making changes

- **Keep it one sketch.** The firmware lives in `pomodoro_box3.ino` on purpose, so it opens cleanly in the Arduino IDE. Add code to the matching `// ---- Section ----` block.
- **Match the surrounding style.** Two-space indent, short functions, and comments that explain *why* rather than *what*.
- **Don't block `loop()`.** Touch, HTTP and WebSocket handling all run there. Use `millis()`-based timers or a FreeRTOS task instead of long `delay()` calls.
- **Colours are `uint16_t` (RGB565).** LovyanGFX treats a `uint32_t` colour as RGB888, which makes colours come out wrong.
- **Keep the API backward compatible.** Existing clients depend on the endpoint names, the status JSON fields and the WebSocket event names. If you add or change any, update the README.
- **Never commit secrets.** `secrets.h` is git-ignored. If you add a new setting, add a placeholder for it to `secrets.example.h` too.

## Testing checklist

There are no automated tests, so please check on a real BOX-3 before opening a pull request:

- [ ] Builds with no new warnings
- [ ] Tap start/pause, hold-to-skip, swipe left/right and the BOOT button all work
- [ ] A phase runs to completion: chime plays, backlight flashes, next phase loads (tip: `/config?focus=1` for a 1-minute test)
- [ ] Screen sleeps after 2 min idle and wakes cleanly on touch and on an API call
- [ ] `/status` and the other endpoints respond; a wrong token gets `401`
- [ ] A WebSocket client with the right key gets status and events; a wrong key is refused
- [ ] The timer still works with Wi-Fi unavailable

## Pull requests

1. Fork the repo and create a branch from `main`.
2. Keep each pull request focused on one change, with a clear description of what changed and how you tested it.
3. Update `README.md` if behaviour, setup or the API changed.

By contributing, you agree that your contributions are licensed under the [MIT License](LICENSE).
