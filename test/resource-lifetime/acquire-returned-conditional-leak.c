// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;

extern void acquire_handle(handle_t* out);

// resource-model: models/resource-lifetime/generic.txt
// The handle escapes on one path and is dropped on the other: the analysis
// reports the possible leak instead of silencing it because of the escape.
handle_t acquire_and_maybe_null(int shouldReturnNull)
{
    handle_t h;
    // at line 14, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' may leave the function without being released
    acquire_handle(&h);
    if (shouldReturnNull)
        return (handle_t)0;
    return h;
}
