/* NOTE(rnp): hacky stuff to work around broken raylib garbage */
#include <raylib_extended.h>
#include "raylib/src/rcore.c"

void *GetPlatformWindowHandle(void)
{
	return (void *)platform.handle;
}
