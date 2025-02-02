#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/*
 *	This is a close relative of the kernel binman but produces
 *	user space binaries and doesn't have common packing magic or all
 *	the magic segments in the kernel
 *
 *	We cannot use the o65 output because it's just broken in the current
 *	ld65 and appears to be unmaintained and unused.
 *
 *	Our input is
 *	file 1:	linked at 0x100 ZP = 0
 *	file 2: linked at 0x200 ZP = 2
 *
 *	A maximum of 255 ZP entries is permitted (and most systems can't fit
 *	that). The relocation data is handled in the usual way. The oddity is
 *	that we rely on a stub relocator that exists in the system because
 *	it doesn't seem possible to write self relocating 6502 code when
 *	the ZP range is not pre-assigned. The relocator itself is called twice.
 *	One relocates ZP ranges the other code/data references.
 *
 *	The same code almost should work on 680x systems except for the
 *	endian issues.
 *
 *
 */

static uint8_t buf[65536];
static uint8_t bufb[65536];
static int32_t bsize, csize, dsize;
static uint16_t numzp;

static void sweep_relocations(void)
{
  /* We link at 0x0100 and 0x0200. Do not relocate the header except for
     the sig vector. In order to identify ZP relocations we generate those at
     0 and 2 base. Thus an offset of 2 is a ZP ref, an offset of 1 is a 16bit
     high byte ref and any other difference is badness */
  uint8_t *base = buf + 0x0110;
  uint8_t *base2 = bufb + 0x0210;
  uint8_t *relptr = buf + csize + dsize + 0x100; /* write relocs into BSS head */
  int relsize;
  int len = csize + dsize;
  int pos = 0x0100;
  int lastrel = 0x0100;

  /* Relocate zero page entries first */
  while(len--) {
    if (*base == *base2) {
      base++;
      base2++;
      pos++;
      continue;
    }
    /* Code/Data reloc - ignore this pass */
    if (*base == *base2 - 1) {
      base++;
      base2++;
      pos++;
      continue;
    }
    if (*base == *base2 - 2) {
      int diff = pos - lastrel;

      /* Track the highest ZP reloc we see - it's zero based so this
         in turn gives us the liit */
      if (numzp < *base)
        numzp = *base + 1;
//      printf("Relocation %d at %x\n", ++rels, pos);
      /* 1 - 254 skip that many and reloc, 255 move on 254, 0 end */
      while(diff > 254) {
          diff -= 254;
          *relptr++ = 255;
      }
      *relptr++ = diff;
      lastrel = pos;
      pos++;
      (*base)-= 2;		/* Zero base the ZP entries */
      base++;
      base2++;
      continue;
    }
    fprintf(stderr, "Bad relocation at %d (%02X v %02X)\n", pos,
      *base, *base2);
    exit(1);
  }
  /* End marker */
  *relptr++ = 0;

  /* Relocation table two is the code/data references */
  while(len--) {
    if (*base == *base2) {
      base++;
      base2++;
      pos++;
      continue;
    }
    /* We shift the binary by 0x100 and the ZP by two so we can find both */
    if (*base == *base2 - 2) {
      base++;
      base2++;
      pos++;
      continue;
    }
    if (*base == *base2 - 1) {
      int diff = pos - lastrel;
//      printf("Relocation %d at %x\n", ++rels, pos);
      /* 1 - 254 skip that many and reloc, 255 move on 254, 0 end */
      while(diff > 254) {
          diff -= 254;
          *relptr++ = 255;
      }
      *relptr++ = diff;
      lastrel = pos;
      pos++;
      (*base)--;
      base++;
      base2++;
      continue;
    }
    fprintf(stderr, "Bad relocation at %d (%02X v %02X)\n", pos,
      *base, *base2);
    exit(1);
  }
  *relptr++ = 0x00;
  relsize = relptr - (buf + csize + dsize + 0x0100);
  /* In effect move the relocations from BSS into DATA */
  dsize += relsize;
  bsize -= relsize;
  /* Corner case - more relocations than data - grow the object size slightly */
  if (bsize < 0)
    bsize = 0;
}

int main(int argc, char *argv[])
{
  FILE *bin;
  uint8_t *bp;
  static uint16_t progload = 0x0100;
  uint16_t relbase;

  if (argc != 4) {
    fprintf(stderr, "%s: <binary1> <binary2> <output>\n", argv[0]);
    exit(1);
  }

  bin = fopen(argv[1], "r");
  if (bin == NULL) {
    perror(argv[1]);
    exit(1);
  }
  if (fread(buf, 1, 65536, bin) == 0) {
    fprintf(stderr, "%s: read error on %s\n", argv[0], argv[1]);
    exit(1);
  }
  fclose(bin);
  bin = fopen(argv[2], "r");
  if (bin == NULL) {
    perror(argv[2]);
    exit(1);
  }
  if (fread(bufb, 1, 65536, bin) == 0) {
    fprintf(stderr, "%s: read error on %s\n", argv[0], argv[2]);
    exit(1);
  }
  fclose(bin);
  
  bin = fopen(argv[3], "w");
  if (bin == NULL) {
    perror(argv[3]);
    exit(1);
  }

  /* Work out sizes from the binary */
  csize = buf[6] + 256 * buf[7];
  dsize = buf[8] + 256 * buf[9];
  bsize = buf[10] + 256 * buf[11];

  /* Offset to start of BSS before adjustment */
  relbase = csize + dsize;

  /* Compute the relocations */
  sweep_relocations();
  
  /* Modify the existing binary header. We touch only the data/bss size */
  buf[3] = 0;	/* Relocatable */
  buf[8] = dsize;
  buf[9] = dsize >> 8;
  buf[10] = bsize;
  buf[11] = bsize >> 8;
  /* We calculate this as it's not known beforehand */
  buf[15] = numzp;
  /* User 16/17 are reserved already for signal vector */
  buf[18] = relbase;
  buf[19] = relbase + 1;
  
  /* Write out everything that is data, omit everything that will 
     be zapped */
  if (fwrite(buf + progload, csize + dsize, 1, bin) != 1) {
   perror(argv[4]);
   exit(1);
  }
  fclose(bin);
  printf("%s: %d bytes.\n", argv[4], csize + dsize);
  exit(0);
}
