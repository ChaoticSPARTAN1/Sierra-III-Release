/* What the engine's sound manager tells the port's mixer (psp_dsound.c)
across the DirectSound calls, for the log's audio lines. The engine side is
psp_fixups.py's FIXES_SOUND, in sound_manager.c. */
#ifndef PSP_SOUND_H
#define PSP_SOUND_H

/* why the game stopped a sound: set around each sound_stop call, read when
the mixer counts a sound cut off before its end */
enum psp_sound_stop_reason
{
	PSP_STOP_OTHER,
	PSP_STOP_INSTANCE_OLDER,  /* the definition's instance limit: an older like sound retired */
	PSP_STOP_INSTANCE_NEWER,  /* the same limit: the new sound itself retired */
	PSP_STOP_SOURCE_LOST,     /* its object or looping track gone */
	PSP_STOP_PLAYERS_DEAD,    /* dialog to the players, with all of them dead */
	PSP_STOP_FADED,           /* its fade reached silence (out of range, a stopped impulse, a loop's end) */
	PSP_STOP_PREEMPTED,       /* its voice taken by a sound of higher priority */
	PSP_STOP_NO_CHANNEL,      /* no voice free for it */
	PSP_STOP_CACHE_MISS,      /* not in the sound cache in time, and its class discards */
	PSP_STOP_ALL,             /* every sound stopped (the map's end) */
	PSP_STOP_REASONS
};

extern int psp_sound_stop_reason;
/* the sound class being queued to a channel, or -1: the stream keeps it, so
a dry gap can say what fell silent */
extern long psp_sound_queueing_class;

#endif
