/*
 * sweetsym: names for shared-cache addresses from an earlier boot, worked out on the phone (2026-11-24).
 *
 *   sweetsym IN OUT
 *
 * IN holds lines "LABEL SLIDE ADDR [ADDR...]" (hex, 0x optional): addresses taken in some boot (the kernel's CRASH
 * and SMP lines print them with that boot's shared-cache slide). Each address is moved to this boot's slide and
 * named with dladdr(): every process maps the same shared cache, so this one's symbols are the crashed process's.
 * Addresses outside the cache (a program's own code) are printed as they are. OUT gets one line per address:
 *   LABEL #N ADDR = unslid UNSLID: symbol+0xOFF (image)
 * Lines starting with '#' and empty lines are skipped. Run by sweetroot (`run /usr/local/bin/sweetsym ...`).
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

int
main(int argc, char **argv)
{
	size_t length = 0;
	const uint8_t *cache = _dyld_get_shared_cache_range(&length);
	uint64_t base, slide;
	char line[4096];
	FILE *in, *out;
	unsigned lines = 0, named = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: sweetsym IN OUT\n");
		return 2;
	}
	if (cache == NULL || length == 0) {
		fprintf(stderr, "sweetsym: no shared cache in this process\n");
		return 1;
	}
	/* the cache's own first mapping says where it would be with no slide */
	uint32_t mapping_offset;
	memcpy(&mapping_offset, cache + 0x10, sizeof(mapping_offset));
	memcpy(&base, cache + mapping_offset, sizeof(base));
	slide = (uint64_t)(uintptr_t)cache - base;
	if ((in = fopen(argv[1], "r")) == NULL || (out = fopen(argv[2], "w")) == NULL) {
		perror("sweetsym: open");
		return 1;
	}
	fprintf(out, "sweetsym: shared cache at %p, unslid base %#llx, slide %#llx, %zu bytes\n", (const void *)cache,
	    (unsigned long long)base, (unsigned long long)slide, length);
	while (fgets(line, sizeof(line), in) != NULL) {
		char *save = NULL, *label, *tok;
		uint64_t their_slide;
		unsigned n = 0;

		if (line[0] == '#' || line[0] == '\n') {
			continue;
		}
		if ((label = strtok_r(line, " \t\r\n", &save)) == NULL || (tok = strtok_r(NULL, " \t\r\n", &save)) == NULL) {
			continue;
		}
		their_slide = strtoull(tok, NULL, 16);
		lines++;
		while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
			uint64_t addr = strtoull(tok, NULL, 16), unslid = addr - their_slide, here = unslid + slide;
			Dl_info info;

			n++;
			if (addr == 0) {
				continue;
			}
			if (here < (uint64_t)(uintptr_t)cache || here >= (uint64_t)(uintptr_t)cache + length) {
				fprintf(out, "%s #%u %#llx: not in the shared cache (the program's own code?)\n", label, n,
				    (unsigned long long)addr);
				continue;
			}
			memset(&info, 0, sizeof(info));
			if (dladdr((const void *)(uintptr_t)here, &info) != 0 && info.dli_sname != NULL) {
				const char *image = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;

				fprintf(out, "%s #%u %#llx = unslid %#llx: %s+%#llx (%s)\n", label, n, (unsigned long long)addr,
				    (unsigned long long)unslid, info.dli_sname,
				    (unsigned long long)(here - (uint64_t)(uintptr_t)info.dli_saddr),
				    image ? image + 1 : (info.dli_fname ? info.dli_fname : "?"));
				named++;
			} else {
				fprintf(out, "%s #%u %#llx = unslid %#llx: (no symbol%s%s)\n", label, n, (unsigned long long)addr,
				    (unsigned long long)unslid, info.dli_fname ? ", in " : "", info.dli_fname ? info.dli_fname : "");
			}
		}
	}
	fprintf(out, "sweetsym: %u line(s), %u address(es) named\n", lines, named);
	fclose(out);
	fclose(in);
	return 0;
}
