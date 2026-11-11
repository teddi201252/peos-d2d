/* OpenAL's context calls for PeOS ports (al_peos.c): one device, PeOS's
 * audio, one context. */
#ifndef PEOS_AL_ALC_H
#define PEOS_AL_ALC_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;
typedef char ALCboolean;
typedef char ALCchar;
typedef int ALCint;
typedef int ALCenum;

#define ALC_FALSE 0
#define ALC_TRUE 1
#define ALC_DEVICE_SPECIFIER 0x1005
#define ALC_DEFAULT_DEVICE_SPECIFIER 0x1004
#define ALC_NO_ERROR 0

ALCdevice *alcOpenDevice(const ALCchar *devicename);
ALCboolean alcCloseDevice(ALCdevice *device);
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attrlist);
void alcDestroyContext(ALCcontext *context);
ALCboolean alcMakeContextCurrent(ALCcontext *context);
const ALCchar *alcGetString(ALCdevice *device, ALCenum param);

#ifdef __cplusplus
}
#endif
#endif
