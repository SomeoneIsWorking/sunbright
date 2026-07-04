// Path A shim: Aurora provides the OS types monolithically in <dolphin/os.h>.
// reference/sms decomp includes the granular subheaders — intercept them here
// so Aurora's definitions win and the decomp's duplicates never fire.
#pragma once
#include <dolphin/os.h>
