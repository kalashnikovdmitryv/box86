#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "sdl2rwops.h"
#include "debug.h"
#include "wrapper.h"
#include "box86context.h"
#include "x86run.h"
#include "x86emu.h"
#include "emu/x86emu_private.h"
#include "librarian/library_private.h"
#include "bridge.h"
#include "callback.h"

typedef struct SDL2_RWops_s SDL2_RWops_t;

typedef int64_t (*sdl2_size)(SDL2_RWops_t *context);
typedef int64_t (*sdl2_seek)(SDL2_RWops_t *context, int64_t offset, int32_t whence);
typedef int32_t (*sdl2_read)(SDL2_RWops_t *context, void *ptr, int32_t size, int32_t maxnum);
typedef int32_t (*sdl2_write)(SDL2_RWops_t *context, const void *ptr, int32_t size, int32_t num);
typedef int32_t (*sdl2_close)(SDL2_RWops_t *context);

#define BOX86RW 0x72 // random signature value

typedef struct SDL2_RWops_s {
    sdl2_size  size;
    sdl2_seek  seek;
    sdl2_read  read;
    sdl2_write write;

    sdl2_close close;

    uint32_t type;
    union {
        struct {
            int autoclose;
            FILE *fp;
        } stdio;
        struct {
            uint8_t *base;
            uint8_t *here;
            uint8_t *stop;
        } mem;
        struct {
            void *data1;
        } unknown;
        struct {
            SDL2_RWops_t *orig;
            sdl2_freerw custom_free;
        } my;
    } hidden;
} SDL2_RWops_t;

EXPORT int64_t my2_native_size(SDL2_RWops_t *context)
{
    return context->hidden.my.orig->size(context->hidden.my.orig);
}
EXPORT int64_t my2_native_seek(SDL2_RWops_t *context, int64_t offset, int32_t whence)
{
    return context->hidden.my.orig->seek(context->hidden.my.orig, offset, whence);
}
EXPORT int32_t my2_native_read(SDL2_RWops_t *context, void *ptr, int32_t size, int32_t maxnum)
{
    return context->hidden.my.orig->read(context->hidden.my.orig, ptr, size, maxnum);
}
EXPORT int32_t my2_native_write(SDL2_RWops_t *context, const void *ptr, int32_t size, int32_t num)
{
    return context->hidden.my.orig->write(context->hidden.my.orig, ptr, size, num);
}
EXPORT int32_t my2_native_close(SDL2_RWops_t *context)
{
    int32_t ret = context->hidden.my.orig->close(context->hidden.my.orig);
    context->hidden.my.custom_free(context);
    return ret;
}
EXPORT int64_t my2_emulated_size(SDL2_RWops_t *context)
{
    return (int64_t)RunFunction64(my_context, (uintptr_t)context->hidden.my.orig->size, 1, context->hidden.my.orig);
}
EXPORT int64_t my2_emulated_seek(SDL2_RWops_t *context, int64_t offset, int32_t whence)
{
    return (int64_t)RunFunction64(my_context, 
        (uintptr_t)context->hidden.my.orig->seek, 4, context->hidden.my.orig, 
        (void*)(uintptr_t)(offset&0xffffffff), 
        (void*)(uintptr_t)((offset>>32)&0xffffffff),
        whence);
}
EXPORT int32_t my2_emulated_read(SDL2_RWops_t *context, void *ptr, int32_t size, int32_t maxnum)
{
    return (int32_t)RunFunction(my_context, (uintptr_t)context->hidden.my.orig->read, 4, context->hidden.my.orig, ptr, size, maxnum);
}
EXPORT int32_t my2_emulated_write(SDL2_RWops_t *context, const void *ptr, int32_t size, int32_t num)
{
    return (int32_t)RunFunction(my_context, (uintptr_t)context->hidden.my.orig->write, 4, context->hidden.my.orig, ptr, size, num);
}
EXPORT int32_t my2_emulated_close(SDL2_RWops_t *context)
{
    return (int32_t)RunFunction(my_context, (uintptr_t)context->hidden.my.orig->close, 1, context->hidden.my.orig);
}

SDL2_RWops_t* AddNativeRW2(x86emu_t* emu, SDL2_RWops_t* ops)
{
    if(!ops)
        return NULL;
    uintptr_t fnc;
    bridge_t* system = emu->context->system;

    sdl2_allocrw Alloc = (sdl2_allocrw)emu->context->sdl2allocrw;
    sdl2_freerw Free = (sdl2_freerw)emu->context->sdl2freerw;

    SDL2_RWops_t* newrw = Alloc();
    newrw->type = BOX86RW;
    newrw->hidden.my.orig = ops;
    newrw->hidden.my.custom_free = Free;

    // get or create wrapper, add it to map and change to the emulated one if rw
    #define GO(A, W) \
    fnc = AddCheckBridge(system, W, my2_native_##A, 0); \
    newrw->A = (sdl2_##A)fnc;

    GO(size, IFp)
    GO(seek, IFpIi)
    GO(read, iFppii)
    GO(write, iFppii)
    GO(close, iFp)

    #undef GO

    return newrw;
}

// put Native RW function, wrapping emulated (callback style) ones if needed
SDL2_RWops_t* RWNativeStart2(x86emu_t* emu, SDL2_RWops_t* ops)
{
    if(!ops)
        return NULL;

    if(ops->type == BOX86RW)
        return ops->hidden.my.orig;

    sdl2_allocrw Alloc = (sdl2_allocrw)emu->context->sdl2allocrw;

    SDL2_RWops_t* newrw = Alloc();
    newrw->type = BOX86RW;
    newrw->hidden.my.orig = ops;
   
    // create wrapper
    #define GO(A, W) \
    newrw->A = my2_emulated_##A;

    GO(size, IFp)
    GO(seek, IFpIi)
    GO(read, iFppii)
    GO(write, iFppii)
    GO(close, iFp)

    #undef GO

    return newrw;
}

void RWNativeEnd2(SDL2_RWops_t* ops)
{
    if(!ops)
        return;

    if(ops->type != BOX86RW)
        return; // do nothing

    ops->hidden.my.custom_free(ops);
}

int64_t RWNativeSeek2(SDL2_RWops_t *ops, int64_t offset, int32_t whence)
{
    return ops->seek(ops, offset, whence);
}
uint32_t RWNativeRead2(SDL2_RWops_t* ops, void* ptr, uint32_t size, uint32_t maxnum)
{
    return ops->read(ops, ptr, size, maxnum);
}
int32_t RWNativeWrite2(SDL2_RWops_t *ops, const void *ptr, int32_t size, int32_t num)
{
    return ops->write(ops, ptr, size, num);
}
int32_t RWNativeClose2(SDL2_RWops_t* ops)
{
    return ops->close(ops);
}