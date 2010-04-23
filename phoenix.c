/*
 * Copyright 2009      Luc Verhaegen <libv@skynet.be>
 * Copyright 2000-2003 Anthony Borisow
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "bios_extract.h"
#include "lh5_extract.h"

struct PhoenixModuleName
{
    char Id;
    char *Name;
};

static struct PhoenixModuleName
PhoenixModuleNames[] = {
    {'A', "acpi"},
    {'B', "bioscode"},
    {'C', "update"},
    {'D', "display"},
    {'E', "setup"},
    {'F', "font"},
    {'G', "decompcode"},
    {'I', "bootblock"},
    {'L', "logo"},
    {'M', "miser"},
    {'N', "rompilotload"},
    {'O', "network"},
    {'P', "rompilotinit"},
    {'R', "oprom"},
    {'S', "strings"},
    {'T', "template"},
    {'U', "user"},
    {'X', "romexec"},
    {'W', "wav"},
    {'H', "tcpa_H"}, /* TCPA (Trusted Computing), USBKCLIB? */
    {'K', "tcpa_K"}, /* TCPA (Trusted Computing), "AUTH"? */
    {'Q', "tcpa_Q"}, /* TCPA (Trusted Computing), "SROM"? */
    {'<', "tcpa_<"},
    {'*', "tcpa_*"},
    {'?', "tcpa_?"},
    {'J', "SmartCardPAS"},
};

static char *
PhoenixModuleNameGet(char Id)
{
    int i;

    for (i = 0; PhoenixModuleNames[i].Name; i++)
	if (PhoenixModuleNames[i].Id == Id)
	    return PhoenixModuleNames[i].Name;

    return NULL;
}

static int
PhoenixModule(unsigned char *BIOSImage, int BIOSLength, int Offset)
{
    struct PhoenixModule {
	uint32_t Previous;
	uint8_t Signature[3];
	uint8_t Id;
	uint8_t Type;
	uint8_t HeadLen;
	uint8_t Compression;
	uint16_t Offset;
	uint16_t Segment;
	uint32_t ExpLen1;
	uint32_t Packed1;
	uint32_t Packed2;
	uint32_t ExpLen2;
    } *Module;
    char *filename, *ModuleName;
    unsigned char *Buffer;
    int fd;

    Module = (struct PhoenixModule *) (BIOSImage + Offset);

    if (Module->Signature[0] || (Module->Signature[1] != 0x31) ||
	(Module->Signature[2] != 0x31)) {
	fprintf(stderr, "Error: Invalid module signature at 0x%05X\n",
		Offset);
	return 0;
    }

    if ((Offset + Module->HeadLen + 4 + le32toh(Module->Packed1)) > BIOSLength) {
	fprintf(stderr, "Error: Module overruns buffer at 0x%05X\n",
		Offset);
	return le32toh(Module->Previous);
    }

    ModuleName = PhoenixModuleNameGet(Module->Type);
    if (ModuleName) {
	filename = malloc(strlen(ModuleName) + 7);
	sprintf(filename, "%s_%1d.rom", ModuleName, Module->Id);
    } else {
	filename = malloc(9);
	sprintf(filename, "%02X_%1d.rom", Module->Type, Module->Id);
    }

    fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
	fprintf(stderr, "Error: unable to open %s: %s\n\n",
		filename, strerror(errno));
	free(filename);
	return Module->Previous;
    }

    switch (Module->Compression) {
    case 5: /* LH5 */
	printf("0x%05X (%6d bytes)   ->   %s\t(%d bytes)", Offset + Module->HeadLen + 4,
	       le32toh(Module->Packed1), filename, le32toh(Module->ExpLen1));

	Buffer = MMapOutputFile(filename, le32toh(Module->ExpLen1));
	if (!Buffer)
	    break;

	LH5Decode(BIOSImage + Offset + Module->HeadLen + 4,
		  le32toh(Module->Packed1), Buffer, le32toh(Module->ExpLen1));

	munmap(Buffer, le32toh(Module->ExpLen1));

	break;
    /* case 3 */ /* LZSS */
    case 0: /* not compressed at all */
	/* why do we not use the full header here? */
	printf("0x%05X (%6d bytes)   ->   %s", Offset + Module->HeadLen,
	       le32toh(Module->Packed1), filename);

	write(fd, BIOSImage + Offset + Module->HeadLen, le32toh(Module->Packed1));
	break;
    default:
	fprintf(stderr, "Unsupported compression type for %s: %d\n",
		filename, Module->Compression);
	printf("0x%05X (%6d bytes)   ->   %s\t(%d bytes)", Offset + Module->HeadLen + 4,
	       le32toh(Module->Packed1), filename, le32toh(Module->ExpLen1));

	write(fd, BIOSImage + Offset + Module->HeadLen + 4, le32toh(Module->Packed1));
	break;
    }

    close(fd);
    free(filename);

    if (le16toh(Module->Offset) || le16toh(Module->Segment)) {
	if (!Module->Compression)
	    printf("\t\t");
	printf("\t [0x%04X:0x%04X]\n", le16toh(Module->Segment) << 12, le16toh(Module->Offset));
    } else
	printf("\n");

    return le32toh(Module->Previous);
}

/*
 *
 */
Bool
PhoenixExtract(unsigned char *BIOSImage, int BIOSLength, int BIOSOffset,
	       uint32_t Offset1, uint32_t BCPSegmentOffset)
{
    struct PhoenixID {
	char Name[6];
	uint16_t Flags;
	uint16_t Length;
    } *ID;
    uint32_t Offset;

    printf("Found Phoenix BIOS \"%s\"\n", (char *) (BIOSImage + Offset1));

    for (ID = (struct PhoenixID *) (BIOSImage + BCPSegmentOffset + 10);
	 ((void *) ID < (void *) (BIOSImage + BIOSLength)) && ID->Name[0];
	 ID = (struct PhoenixID *) (((unsigned char *) ID) + le16toh(ID->Length))) {
#if 0
	printf("PhoenixID: Name %c%c%c%c%c%c, Flags 0x%04X, Length %d\n",
	       ID->Name[0],  ID->Name[1], ID->Name[2],  ID->Name[3],
	       ID->Name[4],  ID->Name[5], le16toh(ID->Flags), le16toh(ID->Length));
#endif
	if (!strncmp(ID->Name, "BCPSYS", 6))
	    break;
    }

    if (!ID->Name[0] || ((void *) ID >= (void *) (BIOSImage + BIOSLength))) {
	fprintf(stderr, "Error: Failed to locate BCPSYS offset.\n");
	return FALSE;
    }

    /* Get some info */
    {
	char Date[9], Time[9], Version[9];

	strncpy(Date, ((char *) ID) + 0x0F, 8);
	Date[8] = 0;
	strncpy(Time, ((char *) ID) + 0x18, 8);
	Time[8] = 0;
	strncpy(Version, ((char *) ID) + 0x37, 8);
	Version[8] = 0;

	printf("Version \"%s\", created on %s at %s.\n", Version, Date, Time);
    }

    Offset = le32toh(*((uint32_t *) (((char *) ID) + 0x77)));
    Offset &= (BIOSLength - 1);
    if (!Offset) {
	fprintf(stderr, "Error: retrieved invalid Modules offset.\n");
	return FALSE;
    }

    while (Offset) {
	Offset = PhoenixModule(BIOSImage, BIOSLength, Offset);
	Offset &= BIOSLength - 1;
    }

    return TRUE;
}

/*
 *
 */
Bool
PhoenixTrustedExtract(unsigned char *BIOSImage, int BIOSLength, int BIOSOffset,
	       uint32_t Offset1, uint32_t BCPSegmentOffset)
{
    fprintf(stderr, "ERROR: Phoenix TrustedCore images are not supported.\n");
    printf("Feel free to RE the decompression routine :)\n");

    return FALSE;
}
