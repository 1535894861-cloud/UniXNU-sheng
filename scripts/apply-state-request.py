#!/usr/bin/env python3
"""iPadOS 27 sleep/wake fix for the sheng port: complete the display state request.

The board's GOP scanout has no panel power switch. The original IOMobileFramebufferLegacy
code answers a display state request (slot 375, applyStateRequest) and expects the board's
platform subclass to write the "powered" word of the state pair. This port only writes it
once at start (announcePoweredByFirmware), so a display-off request leaves requested=0 with
powered=1: the user client's state wait (selector 6) never completes, the compositor's
display-mode transition stays pending, and every later power-button press re-issues "off"
(the panel is darkened by the backlight ramp and never lit again; only a forced reboot
recovers it).

The patch mirrors the requested word into the powered word, so both directions complete,
and logs every request to syslog ([legacy-pm]) so the next boot shows what the OS asked.

Run from the packaging step, before any build: the source belongs to the pinned
UniXNU/GopFramebuffer checkout under the core repository.
"""

import pathlib
import sys

CANDIDATES = (pathlib.Path('UniXNU/drivers/GopFramebuffer/NativeLegacyDisplay.cpp'),
              pathlib.Path('../UniXNU/drivers/GopFramebuffer/NativeLegacyDisplay.cpp'),
              pathlib.Path('drivers/GopFramebuffer/NativeLegacyDisplay.cpp'))

ANCHOR = '    bool start(IOService *provider) override {\n'
MARKER = 'int applyStateRequest(OriginalLegacyStateRequest *request, bool wait) override'

METHOD = '''    // iPadOS 27 display on/off (original state request, slot 375). GOP has no panel power
    // switch, so the powered word only ever mirrors "on": an off request is answered with the
    // panel kept lit, because this board never relights a darkened panel. The registry words
    // (LegacyPM*) let the host see whether this virtual is reached at all, since kernel IOLog
    // does not reach the user-space syslog here.
    int applyStateRequest(OriginalLegacyStateRequest *request, bool wait) override {
        int status = IOMobileFramebufferLegacy::applyStateRequest(request, wait);
        if (!request) return status;
        IOLog("[legacy-pm] request skip=%u/%u requested=%u powered=%u changed=%u status=%d\\n",
              request->skipRequested, request->skipPowered, request->requested,
              request->powered, (unsigned)request->changed, status);
        OSNumber *pmRequested = OSNumber::withNumber((unsigned long long)request->requested, 32);
        if (pmRequested) {setProperty("LegacyPMRequested",pmRequested);pmRequested->release();}
        OSNumber *pmPowered = OSNumber::withNumber((unsigned long long)request->powered, 32);
        if (pmPowered) {setProperty("LegacyPMPowered",pmPowered);pmPowered->release();}
        OSNumber *pmStatus = OSNumber::withNumber((unsigned long long)(unsigned)status, 32);
        if (pmStatus) {setProperty("LegacyPMStatus",pmStatus);pmStatus->release();}
        if (!request->skipRequested && !request->skipPowered && request->requested != request->powered) {
            OriginalLegacyStateRequest mirror = *request;
            mirror.skipRequested = 1;
            mirror.skipPowered = 0;
            mirror.powered = 1;
            mirror.changed = 1;
            status = IOMobileFramebufferLegacy::applyStateRequest(&mirror, wait);
            OSNumber *pmMirror = OSNumber::withNumber((unsigned long long)1, 32);
            if (pmMirror) {setProperty("LegacyPMMirror",pmMirror);pmMirror->release();}
            IOLog("[legacy-pm] mirrored powered=%u status=%d\\n", mirror.powered, status);
        }
        return status;
    }
'''


def main():
    found = [p for p in CANDIDATES if p.is_file()]
    if not found:
        sys.exit('no display source found; tried: ' + ', '.join(str(p) for p in CANDIDATES))
    for source in found:
        text = source.read_text()
        if MARKER in text:
            print(f'already patched: {source}')
            continue
        if text.count(ANCHOR) != 1:
            sys.exit(f'anchor count {text.count(ANCHOR)} in {source}')
        source.write_text(text.replace(ANCHOR, METHOD + ANCHOR))
        print(f'patched {source} (+{METHOD.count(chr(10))} lines)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
