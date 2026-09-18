/**
 * detect.cpp — the board's one self-assertion.
 *
 * The hardware under this firmware is the host process itself, so the answer
 * is unconditional. There is nothing to probe and no bus to drive, which is
 * why detect_probe.h — the probe helpers, all of them chip peripherals — is
 * not included here.
 */

extern "C" const char* detect_hw(void)
{
    return "hw-linux";
}
