/*
halo_loadtest: does a Halo map built for a PSP address load at that address?

Reads ms0:/halo/b30.map (uncompressed, built with invader-build -X), reserves
the 22 MB tag window at the address the map was built for, reads the tag data
and each BSP into it, and checks that every pointer lands inside the window.
Results go to the screen and to ms0:/halo_loadtest.txt.
*/

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

PSP_MODULE_INFO("halo_loadtest", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(256);

#define MAP_PATH "ms0:/halo/b30.map"
#define LOG_PATH "ms0:/halo_loadtest.txt"
#define TAG_SPACE 0x01600000u
#define XBOX_BASE 0x803A6000u

static SceUID log_fd = -1;

static void out(const char *format, ...)
{
	char line[256];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (pspDebugScreenGetY() >= 32)
		pspDebugScreenClear();
	pspDebugScreenPrintf("%s", line);
	if (log_fd >= 0)
		sceIoWrite(log_fd, line, strlen(line));
}

static int exit_callback(int a, int b, void *c)
{
	sceKernelExitGame();
	return 0;
}

static int callback_thread(SceSize args, void *argp)
{
	int id = sceKernelCreateCallback("exit", exit_callback, NULL);

	sceKernelRegisterExitCallback(id);
	sceKernelSleepThreadCB();
	return 0;
}

static unsigned long crc_table[256];

static unsigned long crc32_buffer(unsigned long crc, const unsigned char *data, unsigned long size)
{
	unsigned long i;

	crc = ~crc;
	for (i = 0; i < size; i++)
		crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static void crc_init(void)
{
	unsigned long n, k, c;

	for (n = 0; n < 256; n++)
	{
		c = n;
		for (k = 0; k < 8; k++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

/* reads size bytes at offset into destination, in 1 MB pieces; returns bytes read */
static long read_at(SceUID fd, unsigned long offset, void *destination, unsigned long size)
{
	unsigned long done = 0;

	sceIoLseek32(fd, offset, PSP_SEEK_SET);
	while (done < size)
	{
		unsigned long piece = size - done > 0x100000 ? 0x100000 : size - done;
		int got = sceIoRead(fd, (char *)destination + done, piece);

		if (got <= 0)
			break;
		done += got;
	}
	return done;
}

static void wait_for_exit(void)
{
	SceCtrlData pad;

	out("\nPress START to exit.\n");
	if (log_fd >= 0)
	{
		sceIoClose(log_fd);
		log_fd = -1;
	}
	for (;;)
	{
		sceCtrlReadBufferPositive(&pad, 1);
		if (pad.Buttons & PSP_CTRL_START)
			break;
		sceKernelDelayThread(50000);
	}
	sceKernelExitGame();
}

int main(void)
{
	static unsigned long header[0x200];
	SceUID fd, block;
	unsigned long tag_offset, tag_size, base, *tag_header, count, i;
	unsigned long names_in = 0, names_out = 0, data_in = 0, data_zero = 0, data_out = 0;
	unsigned long words_window = 0, words_xbox = 0;
	unsigned long scenario_index, scenario_data, bsp_count, bsp_pointer;
	SceInt64 t0, t1;
	long got;

	pspDebugScreenInit();
	sceKernelStartThread(sceKernelCreateThread("callbacks", callback_thread, 0x11, 0xFA0, 0, 0), 0, 0);
	log_fd = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	crc_init();
	out("halo_loadtest v1: %s\n", MAP_PATH);
	out("free user memory: %d KB total, %d KB largest block\n",
		sceKernelTotalFreeMemSize() / 1024, sceKernelMaxFreeMemSize() / 1024);

	fd = sceIoOpen(MAP_PATH, PSP_O_RDONLY, 0);
	if (fd < 0)
	{
		out("FAIL: can't open the map (0x%08X)\n", fd);
		wait_for_exit();
	}
	read_at(fd, 0, header, sizeof(header));
	if (header[0] != 0x68656164 || header[0x7FC / 4] != 0x666F6F74)
	{
		out("FAIL: not an uncompressed map ('head'/'foot' missing). Run mapunpack.py on it.\n");
		wait_for_exit();
	}
	tag_offset = header[0x10 / 4];
	tag_size = header[0x14 / 4];
	out("map '%s': tag data %lu bytes at file offset 0x%lX\n", (char *)header + 0x20, tag_size, tag_offset);

	/* the first word of the tag data is the tag array's address; the array
	follows the 0x24-byte header, so the window starts 0x24 below it */
	read_at(fd, tag_offset, header, 4);
	base = header[0] - 0x24;
	out("built for 0x%08lX; window 0x%08lX-0x%08lX\n", base, base, base + TAG_SPACE);
	block = sceKernelAllocPartitionMemory(2, "halo tags", PSP_SMEM_Addr, TAG_SPACE, (void *)base);
	if (block < 0)
	{
		out("FAIL: can't reserve the window at 0x%08lX (0x%08X)\n", base, block);
		wait_for_exit();
	}
	out("reserved at 0x%08lX\n", (unsigned long)sceKernelGetBlockHeadAddr(block));

	t0 = sceKernelGetSystemTimeWide();
	got = read_at(fd, tag_offset, (void *)base, tag_size);
	t1 = sceKernelGetSystemTimeWide();
	out("read %ld bytes in %d ms (%.1f MB/s)\n", got, (int)((t1 - t0) / 1000),
		got / 1048576.0 / ((t1 - t0) / 1000000.0));
	out("crc32 of tag data: 0x%08lX\n", crc32_buffer(0, (unsigned char *)base, tag_size));

	tag_header = (unsigned long *)base;
	count = tag_header[3];
	out("tag header: signature %s, %lu tags, scenario 0x%08lX\n",
		tag_header[8] == 0x74616773 ? "'tags' OK" : "WRONG", count, tag_header[1]);
	for (i = 0; i < count; i++)
	{
		unsigned long *instance = (unsigned long *)(tag_header[0] + i * 0x20);
		unsigned long name = instance[4], data = instance[5];

		if (name >= base && name < base + tag_size) names_in++; else names_out++;
		if (data == 0) data_zero++;
		else if (data >= base && data < base + tag_size) data_in++;
		else data_out++;
	}
	out("tag names: %lu inside, %lu outside\n", names_in, names_out);
	out("tag data: %lu inside, %lu not loaded (BSPs), %lu outside\n", data_in, data_zero, data_out);

	for (i = 0; i < tag_size / 4; i++)
	{
		unsigned long word = ((unsigned long *)base)[i];

		if (word >= base && word < base + TAG_SPACE) words_window++;
		if (word >= XBOX_BASE && word < XBOX_BASE + TAG_SPACE) words_xbox++;
	}
	out("words that look like addresses: %lu in the window, %lu in the Xbox window\n",
		words_window, words_xbox);

	scenario_index = tag_header[1] & 0xFFFF;
	scenario_data = ((unsigned long *)(tag_header[0] + scenario_index * 0x20))[5];
	bsp_count = *(unsigned long *)(scenario_data + 0x5A4);
	bsp_pointer = *(unsigned long *)(scenario_data + 0x5A8);
	out("%lu BSPs\n", bsp_count);
	for (i = 0; i < bsp_count; i++)
	{
		unsigned long *reference = (unsigned long *)(bsp_pointer + i * 0x20);
		unsigned long file_offset = reference[0], size = reference[1], address = reference[2];
		unsigned long *bsp_header;
		int fits = address >= base + tag_size && address + size <= base + TAG_SPACE;

		out(" BSP %lu: %lu KB to 0x%08lX, %s", i, size / 1024, address,
			fits ? "fits" : "OUTSIDE THE WINDOW\n");
		if (!fits)
			continue;
		t0 = sceKernelGetSystemTimeWide();
		got = read_at(fd, file_offset, (void *)address, size);
		t1 = sceKernelGetSystemTimeWide();
		bsp_header = (unsigned long *)address;
		out(", read in %d ms, signature %s, base 0x%08lX %s\n", (int)((t1 - t0) / 1000),
			bsp_header[5] == 0x73627370 ? "OK" : "WRONG", bsp_header[0],
			bsp_header[0] >= address && bsp_header[0] < address + size ? "inside" : "OUTSIDE");
	}
	sceIoClose(fd);

	out("\n%s\n", names_out || data_out || tag_header[8] != 0x74616773 ?
		"RESULT: problems above" : "RESULT: the map loads at its address and every checked pointer is inside");
	wait_for_exit();
	return 0;
}
