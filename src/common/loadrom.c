/******************************************************************************

	loadrom.c

	ROM Image File Load Functions

******************************************************************************/

#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include "emumain.h"

void swab(const void *restrict src, void *restrict dest, ssize_t nbytes);

#if (EMU_SYSTEM != NCDZ)

/******************************************************************************
	Local Variables
******************************************************************************/

static int64_t rom_fd = -1;


/******************************************************************************
	ROM File Reading
******************************************************************************/

/*--------------------------------------------------------
	Search and Open File from ZIP File
--------------------------------------------------------*/

int64_t file_open(const char *fname1, const char *fname2, const uint32_t crc, char *fname)
{
	int i, found = 0;
	struct zip_find_t file;
	char path[PATH_MAX];

	for (i = 0; i < 3; i++)
	{
		switch (i)
		{
		case 0:
            if (!fname1) continue;
#ifdef LIBRETRO
            if (!strcmp(fname1, game_name)) snprintf(path, sizeof(path), "%s", lr_content_archive());
            else
#endif
            snprintf(path, sizeof(path), "%s/%s.zip", game_dir, fname1);
            break;
		case 1: if (!fname2 || !*fname2) continue; snprintf(path, sizeof(path), "%s/%s.zip", game_dir, fname2); break;
		case 2:
#ifdef LIBRETRO
            { const char *bios = fname2 && *fname2 ? fname2 : fname1;
              if (!bios || strcmp(bios, "neogeo")) continue;
              char relative[64]; snprintf(relative, sizeof(relative), "%s.zip", bios);
              snprintf(path, sizeof(path), "%s", lr_system_path(relative)); }
#else
            if (!fname2) continue;
            sprintf(path, "%sroms/%s.zip", launchDir, fname2);
#endif
            break;
		}

		#ifndef LIBRETRO
		printf("FOPEN_TRY: %s game_dir=%s launchDir=%s%c", path, game_dir, launchDir, 10);
#endif
		if (zip_open(path) != -1)
		{
			if (zip_findfirst(&file))
			{
				if (file.crc32 == crc)
				{
					found = 1;
				}
				else
				{
					if (!found)
					{
						while (zip_findnext(&file))
						{
							if (file.crc32 == crc)
							{
								found = 1;
								break;
							}
						}
					}
				}
			}

			if (!found)
			{
				if (fname)
				{
					int64_t fd;

					if ((fd = zopen(fname)) != -1)
					{
						zclose(fd);
						found = 2;
					}
				}
				zip_close();
			}
		}

		if (found) break;
	}

	if (found == 1)
	{
		/* fname is the caller's expected name and may be a small BIOS array.
		 * Open the CRC-matched member directly without overwriting it. */
		rom_fd = zopen(file.name);
		/* Callers need status, not the private ZIP pointer (which may truncate). */
		return rom_fd == -1 ? -1 : 0;
	}
	else if (found == 2)
	{
		return -2;	// CRC error
	}

	return -1;	// not found
}


/*--------------------------------------------------------
	Close File
--------------------------------------------------------*/

void file_close(void)
{
	if (rom_fd != -1)
	{
		zclose(rom_fd);
		zip_close();
		rom_fd = -1;
	}
}


/*--------------------------------------------------------
	Read Specified Bytes from File
--------------------------------------------------------*/

size_t file_read(void *buf, size_t length)
{
	if (rom_fd != -1)
		return zread(rom_fd, buf, length);
	return -1;
}


/*--------------------------------------------------------
	Read 1 Character from File
--------------------------------------------------------*/

int file_getc(void)
{
	if (rom_fd != -1)
		return zgetc(rom_fd);
	return -1;
}


/*--------------------------------------------------------
	Open Cache File
--------------------------------------------------------*/

#if USE_CACHE && (EMU_SYSTEM == MVS)
int cachefile_open(int type)
{
	int32_t fd = -1;
	char path[PATH_MAX];

	switch (type)
	{
	case CACHE_INFO:
		if (use_parent_crom && use_parent_srom && use_parent_vrom)
		{
			sprintf(path, "%s/%s_cache/cache_info", cache_dir, parent_name);
			fd = open(path, O_RDONLY, 0777);
		}
		else
		{
			sprintf(path, "%s/%s_cache/cache_info", cache_dir, game_name);
			fd = open(path, O_RDONLY, 0777);
		}
		break;

	case CACHE_CROM:
		if (use_parent_crom)
		{
			sprintf(path, "%s/%s_cache/crom", cache_dir, parent_name);
			fd = open(path, O_RDONLY, 0777);
		}
		if (fd < 0)
		{
			sprintf(path, "%s/%s_cache/crom", cache_dir, game_name);
			fd = open(path, O_RDONLY, 0777);
		}
		break;

	case CACHE_SROM:
		if (use_parent_srom)
		{
			sprintf(path, "%s/%s_cache/srom", cache_dir, parent_name);
			fd = open(path, O_RDONLY, 0777);
		}
		if (fd < 0)
		{
			sprintf(path, "%s/%s_cache/srom", cache_dir, game_name);
			fd = open(path, O_RDONLY, 0777);
		}
		break;

	case CACHE_VROM:
		if (use_parent_vrom)
		{
			sprintf(path, "%s/%s_cache/vrom", cache_dir, parent_name);
			fd = open(path, O_RDONLY, 0777);
		}
		if (fd < 0)
		{
			sprintf(path, "%s/%s_cache/vrom", cache_dir, game_name);
			fd = open(path, O_RDONLY, 0777);
		}
		break;
	}

	return fd;
}
#endif


/*--------------------------------------------------------
	Load ROM
--------------------------------------------------------*/

int rom_load(struct rom_t *rom, uint8_t *mem, int idx, int max)
{
	uint32_t offset, length;

_continue:
	offset = rom[idx].offset;

	if (rom[idx].skip == 0)
	{
		#ifdef LIBRETRO
        if (file_read(&mem[offset], rom[idx].length) != rom[idx].length) {
            fatalerror("Truncated ROM member: %s", rom[idx].name);
            return max;
        }
#else
        file_read(&mem[offset], rom[idx].length);
#endif

		if (rom[idx].type == ROM_WORDSWAP)
			swab(&mem[offset], &mem[offset], rom[idx].length);
	}
	else
	{
		int c;
		int skip = rom[idx].skip + rom[idx].group;

		length = 0;

		if (rom[idx].group == 1)
		{
			if (rom[idx].type == ROM_WORDSWAP)
				offset ^= 1;

			while (length < rom[idx].length)
			{
				if ((c = file_getc()) == EOF) break;
				mem[offset] = c;
				offset += skip;
				length++;
			}
		}
		else
		{
			while (length < rom[idx].length)
			{
				if ((c = file_getc()) == EOF) break;
				mem[offset + 0] = c;
				if ((c = file_getc()) == EOF) break;
				mem[offset + 1] = c;
				offset += skip;
				length += 2;
			}
		}
#ifdef LIBRETRO
		if (length != rom[idx].length)
		{
			fatalerror("Truncated interleaved ROM member: %s", rom[idx].name);
			return max;
		}
#endif
	}

	if (++idx != max)
	{
		if (rom[idx].type == ROM_CONTINUE)
		{
			goto _continue;
		}
	}

	return idx;
}

#endif /* EMU_SYSTEM */


/******************************************************************************
	Display Error Messages
******************************************************************************/

/*------------------------------------------------------
	Display Memory Allocation Error Message
------------------------------------------------------*/

void error_memory(const char *mem_name)
{
	zip_close();
	msg_printf(TEXT(COULD_NOT_ALLOCATE_x_MEMORY), mem_name);
	msg_printf(TEXT(PRESS_ANY_BUTTON2));
	pad_wait_press(PAD_WAIT_INFINITY);
	Loop = LOOP_BROWSER;
}


/*------------------------------------------------------
	Display CRC Error Message
------------------------------------------------------*/

void error_crc(const char *rom_name)
{
	zip_close();
	msg_printf(TEXT(CRC32_NOT_CORRECT_x), rom_name);
	msg_printf(TEXT(PRESS_ANY_BUTTON2));
	pad_wait_press(PAD_WAIT_INFINITY);
	Loop = LOOP_BROWSER;
}


/*------------------------------------------------------
	Display ROM File Error Message
------------------------------------------------------*/

void error_file(const char *rom_name)
{
	zip_close();
	msg_printf(TEXT(FILE_NOT_FOUND_x), rom_name);
	msg_printf(TEXT(PRESS_ANY_BUTTON2));
	pad_wait_press(PAD_WAIT_INFINITY);
	Loop = LOOP_BROWSER;
}
