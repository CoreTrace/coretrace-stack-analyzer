// case_callback.c
typedef void (*cb_t)(char *);

void use_callback(cb_t cb)
{
    char buf[10];
    cb(buf);  // fuite potentielle via callback
}
