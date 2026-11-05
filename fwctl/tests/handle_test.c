#include <assert.h>
#include "dm_handle.h"
int main(void) {
    uint32_t generation = 0;
    int old_token, new_token;
    old_token = dm_handle_allocate(DM_HANDLE_FW, 0, &generation);
    new_token = dm_handle_allocate(DM_HANDLE_FW, 0, &generation);
    assert(!dm_handle_matches(old_token, DM_HANDLE_FW, 0, generation));
    assert(dm_handle_matches(new_token, DM_HANDLE_FW, 0, generation));
    assert(!dm_handle_matches(new_token, DM_HANDLE_FW, 1, generation));
    generation = DM_HANDLE_GENERATION_MAX;
    assert(!dm_handle_allocate(DM_HANDLE_FW, 0, &generation));
    return 0;
}
