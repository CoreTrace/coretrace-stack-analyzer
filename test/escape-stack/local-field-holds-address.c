// SPDX-License-Identifier: Apache-2.0
//
// The address of a buffer is stored into one field of another local (#181). Only the bytes that
// received it give it back: a pointer read from another field of the same local is not that
// address. When the two accesses overlap, or the position of one of them is not known, the read
// may give the address back.

struct state
{
    char* owned;
    void* table;
};

struct fstate;

struct lstate
{
    struct fstate* fs;
    void* table;
};

struct fstate
{
    struct lstate* ls;
};

union slot
{
    char* text;
    void* any;
};

struct __attribute__((packed)) shifted
{
    int tag;
    char* tail;
};

union overlay
{
    char* head;
    struct shifted shifted;
};

void* make_table(void);

static void own(struct state* s, char* p)
{
    s->owned = p;
}

static void link_states(struct lstate* ls, struct fstate* fs)
{
    ls->fs = fs;
    fs->ls = ls;
}

// *out receives the pointer read from s.table; the address of other_buf is in s.owned.
void publish_other_field(void** out)
{
    char other_buf[16] = {0};
    struct state s;
    s.owned = other_buf;
    s.table = make_table();
    *out = s.table;
}

// The same, with the address stored into s.owned by a callee.
void publish_other_field_after_callee(void** out)
{
    char callee_other_buf[16] = {0};
    struct state s;
    own(&s, callee_other_buf);
    s.table = make_table();
    *out = s.table;
}

// link_states stores the address of each local into the other; *top receives lexstate.table.
void publish_table_of_linked_states(void** top)
{
    struct lstate lexstate;
    struct fstate funcstate;
    lexstate.table = make_table();
    *top = lexstate.table;
    link_states(&lexstate, &funcstate);
}

// *out receives s.owned, which holds the address of owned_buf.
void publish_owned_field(char** out)
{
    char owned_buf[16] = {0};
    struct state s;
    s.owned = owned_buf;
    s.table = make_table();
    *out = s.owned;
}

// The same, with the address stored into s.owned by a callee.
void publish_owned_field_after_callee(char** out)
{
    char callee_owned_buf[16] = {0};
    struct state s;
    own(&s, callee_owned_buf);
    *out = s.owned;
}

// The element read cannot be told apart from the one that holds the address of element_buf.
void publish_unknown_element(char** out, int i, int j)
{
    char element_buf[16] = {0};
    char* slots[2] = {0, 0};
    slots[i] = element_buf;
    *out = slots[j];
}

// The address of union_buf is read back through another member, at the same bytes.
void publish_through_union(void** out)
{
    char union_buf[16] = {0};
    union slot s;
    s.text = union_buf;
    *out = s.any;
}

// The pointer read at byte 4 overlaps the one stored at byte 0.
void publish_overlapping_bytes(char** out)
{
    char overlap_buf[16] = {0};
    union overlay o;
    o.shifted.tag = 0;
    o.shifted.tail = 0;
    o.head = overlap_buf;
    *out = o.shifted.tail;
}

// The address of unknown_store_buf is stored at a position that is not known.
void publish_after_unknown_store(void** out, long k)
{
    char unknown_store_buf[16] = {0};
    struct state s;
    s.table = make_table();
    *(char**)((char*)&s + k) = unknown_store_buf;
    *out = s.table;
}

// at line 95, column 10
// [ !!Warn ] stack pointer escape: address of variable 'owned_buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
//          ↳ destination pointer/value name: 'out'

// at line 104, column 10
// [ !!Warn ] stack pointer escape: address of variable 'callee_owned_buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
//          ↳ destination pointer/value name: 'out'

// at line 113, column 10
// [ !!Warn ] stack pointer escape: address of variable 'element_buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
//          ↳ destination pointer/value name: 'out'

// at line 122, column 10
// [ !!Warn ] stack pointer escape: address of variable 'union_buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
//          ↳ destination pointer/value name: 'out'

// at line 133, column 10
// [ !!Warn ] stack pointer escape: address of variable 'overlap_buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
//          ↳ destination pointer/value name: 'out'

// at line 143, column 10
// [ !!Warn ] stack pointer escape: address of variable 'unknown_store_buf' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
//          ↳ destination pointer/value name: 'out'

// strict-expectation-details: true
