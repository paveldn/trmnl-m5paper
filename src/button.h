#pragma once

enum class WakePress { NONE, CLICK, MEDIUM, LONG, LONGEST };

bool handleBootButtonReset();
void checkRuntimeReset();
WakePress detectButtonWakePress();
