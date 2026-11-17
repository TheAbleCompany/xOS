/*
 * music_artyom.c -- заглушка фоновой музыки для ArtyomX64.
 *
 * Заменяет common/snd_music.c: там нужны stb_vorbis / dr_mp3 / dr_flac
 * (их нет в дереве TyrQuake), а звука в этой сборке всё равно нет
 * (snd_null.c).  Оставляем только символы, которые дергают
 * snd_dma.c / snd_mix.c / cd_common.c.
 */

#include "quakedef.h"
#include "sound.h"

void
S_Music_RegisterVariables()
{
}

void
S_Music_AddCommands()
{
}

void
S_Music_Init()
{
}

void
BGM_Play(const char *filename, qboolean looping)
{
    (void)filename;
    (void)looping;
}

void
BGM_PlayCDTrack(byte track, qboolean looping)
{
    (void)track;
    (void)looping;
}

void
BGM_Stop()
{
}

void
BGM_ClearBuffers()
{
}

void
S_PaintMusic(portable_samplepair_t *buffer, int numsamples)
{
    (void)buffer;
    (void)numsamples;
}
