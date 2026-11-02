// Android runtime permissions from the native thread (ar_quest): Activity.requestPermissions and
// Context.checkSelfPermission through JNI. Attaches the calling thread for the call (and detaches it again when it was
// not attached before).
#pragma once

struct android_app;

namespace ar {
bool qx_perm_granted(android_app *app, const char *perm);   // checkSelfPermission(perm) == PERMISSION_GRANTED
bool qx_perm_request(android_app *app, const char *perm);   // requestPermissions({perm}, 1); true when the call was made
}
