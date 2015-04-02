#ifndef _AL_ERROR_H_
#define _AL_ERROR_H_

#include "alMain.h"

#ifdef __cplusplus
extern "C" {
#endif

extern ALboolean TrapALError;

inline void alDebugBreak()
{
#ifdef _WIN32
    /* DebugBreak will cause an exception if there is no debugger */
    if(IsDebuggerPresent())
    {
        __debugbreak();
    }
#elif defined(SIGTRAP)
    raise(SIGTRAP);
#endif
}

ALvoid alSetError(ALCcontext *Context, ALenum errorCode);

#define SET_ERROR_AND_RETURN(ctx, err) do {                                    \
    alSetError((ctx), (err));                                                  \
    return;                                                                    \
} while(0)

#define SET_ERROR_AND_RETURN_VALUE(ctx, err, val) do {                         \
    alSetError((ctx), (err));                                                  \
    return (val);                                                              \
} while(0)

#define SET_ERROR_AND_GOTO(ctx, err, lbl) do {                                 \
    alSetError((ctx), (err));                                                  \
    goto lbl;                                                                  \
} while(0)

#ifdef __cplusplus
}
#endif

#endif
