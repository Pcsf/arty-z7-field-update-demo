#include "updater.c"
#include <string.h>

static int fails = 0, ran = 0;

static void t(const char *name, const char *text, man_result_t want)
{
	manifest_t m;
	man_result_t got;
	memset(&m, 0, sizeof(m));
	ran++;
	got = manifest_parse((const u8 *)text, (u32)strlen(text), &m);
	if (got != want) {
		printf("FAIL %-38s want %d got %d (%s)\n", name, want, got,
		       man_result_str(got));
		fails++;
	} else {
		printf("ok   %-38s\n", name);
	}
}

/* Build a minimal boot header with the three words 4b inspects, then assert
 * image_is_bootable()'s verdict. */
static void hdr(const char *name, u32 vector, u32 width, u32 ident, int want)
{
	u8 img[BOOTHDR_MIN_LEN];
	int got;
	unsigned i;

	memset(img, 0, sizeof(img));
	for (i = 0; i < 4U; i++) {
		img[BOOTHDR_VECTOR_OFF + i] = (u8)(vector >> (8U * i));
		img[BOOTHDR_WIDTH_OFF  + i] = (u8)(width  >> (8U * i));
		img[BOOTHDR_IDENT_OFF  + i] = (u8)(ident  >> (8U * i));
	}

	ran++;
	got = image_is_bootable(img, (u32)sizeof(img));
	if (got != want) {
		printf("FAIL %-38s want %d got %d\n", name, want, got);
		fails++;
	} else {
		printf("ok   %-38s\n", name);
	}
}

#define GOOD "manifest 1\nsha256 " \
  "ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78" \
  "\nlength 4297288\nversion 0x00020001\n"

int main(void)
{
	manifest_t m;

	t("canonical",                 GOOD,                             MAN_OK);
	t("no trailing newline",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\nversion 0x00020001", MAN_OK);
	t("CRLF",
	  "manifest 1\r\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\r\nlength 4297288\r\nversion 0x00020001\r\n", MAN_OK);
	t("reordered keys",
	  "manifest 1\nversion 0x00020001\nlength 4297288\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\n", MAN_OK);
	t("blank lines tolerated",
	  "manifest 1\n\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\n\nlength 4297288\nversion 0x00020001\n", MAN_OK);
	t("extra spaces after key",
	  "manifest 1\nsha256    ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength   4297288\nversion  0x00020001\n", MAN_OK);

	/* The old two-line format must be refused, and named. */
	t("old positional format",
	  "ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\n4297288\n",
	  MAN_ERR_FORMAT);
	t("empty",                     "",                               MAN_ERR_FORMAT);
	t("future format version",
	  "manifest 2\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\nversion 0x00020001\n",
	  MAN_ERR_FORMAT_VER);
	t("unknown key refused",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\nversion 0x00020001\nsignature deadbeef\n",
	  MAN_ERR_UNKNOWN_KEY);
	t("duplicate key",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\nlength 99\nversion 0x00020001\n",
	  MAN_ERR_DUPLICATE);
	t("missing version",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\n",
	  MAN_ERR_MISSING);
	t("missing sha",
	  "manifest 1\nlength 4297288\nversion 0x00020001\n",             MAN_ERR_MISSING);
	t("short sha",
	  "manifest 1\nsha256 ef4ed80\nlength 4297288\nversion 0x00020001\n", MAN_ERR_VALUE);
	t("non-hex sha",
	  "manifest 1\nsha256 zf4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\nversion 0x00020001\n",
	  MAN_ERR_VALUE);
	t("version without 0x",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4297288\nversion 20001\n",
	  MAN_ERR_VALUE);
	t("length not decimal",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 4x97288\nversion 0x00020001\n",
	  MAN_ERR_VALUE);
	t("length overflows u32",
	  "manifest 1\nsha256 ef4ed8036083804953018181feae23717fa1d214aafe2f65083b45a240978c78\nlength 99999999999999\nversion 0x00020001\n",
	  MAN_ERR_VALUE);
	t("key with no value",
	  "manifest 1\nsha256\nlength 4297288\nversion 0x00020001\n",     MAN_ERR_SYNTAX);

	/* Values actually land where they belong. */
	ran++;
	memset(&m, 0, sizeof(m));
	if (manifest_parse((const u8 *)GOOD, (u32)strlen(GOOD), &m) != MAN_OK ||
	    m.length != 4297288U || m.version != 0x00020001U ||
	    m.digest[0] != 0xefU || m.digest[31] != 0x78U) {
		printf("FAIL %-38s length=%u version=0x%08x d0=%02x d31=%02x\n",
		       "values parsed correctly", (unsigned)m.length,
		       (unsigned)m.version, m.digest[0], m.digest[31]);
		fails++;
	} else {
		printf("ok   %-38s\n", "values parsed correctly");
	}

	/* ── Boot header: step 4b, the gate that keeps a non-image off the slot ──
	 *
	 * The wrong-family case is the one worth a test. bootgen takes -arch on
	 * trust and reports success either way, so a ZynqMP build of this .bif is
	 * an ordinary mistake -- and it carries the same width and identification
	 * magics, differing only in the vector table. */
	hdr("bootable: real Zynq-7000 header", 0xEAFFFFFEU, 0xAA995566U, 0x584C4E58U, 1);
	hdr("reject: ZynqMP vector table",     0x14000000U, 0xAA995566U, 0x584C4E58U, 0);
	hdr("reject: zeroed vector table",     0x00000000U, 0xAA995566U, 0x584C4E58U, 0);
	hdr("reject: bad width magic",         0xEAFFFFFEU, 0xDEADBEEFU, 0x584C4E58U, 0);
	hdr("reject: bad identification",      0xEAFFFFFEU, 0xAA995566U, 0xDEADBEEFU, 0);

	/* Too short to hold the header at all. */
	ran++;
	{
		u8 tiny[BOOTHDR_MIN_LEN];
		memset(tiny, 0, sizeof(tiny));
		if (image_is_bootable(tiny, BOOTHDR_MIN_LEN - 1U) != 0) {
			printf("FAIL %-38s accepted a truncated image\n", "reject: shorter than the header");
			fails++;
		} else {
			printf("ok   %-38s\n", "reject: shorter than the header");
		}
	}

	printf("\n%d/%d passed\n", ran - fails, ran);
	return fails != 0;
}
