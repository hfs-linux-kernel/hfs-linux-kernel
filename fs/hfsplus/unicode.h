/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unicode related declarations
 */

#ifndef _LINUX_HFSPLUS_UNICODE_H
#define _LINUX_HFSPLUS_UNICODE_H

/*
 * struct hfsplus_ccc_range - one run of consecutive BMP code points that
 *                            share the same Unicode canonical combining class
 * @first: the first UTF-16 code point in the run
 * @last: the last code point in the run (inclusive)
 * @combining_class: canonical combining class (ccc) shared by @first..@last
 */
struct hfsplus_ccc_range {
	u16 first;
	u16 last;
	u8 combining_class;
};

/*
 * struct hfsplus_legacy_decomp - corrected canonical decomposition
 * @uc: the code point (a UTF-16 code unit) to look up
 * @len: number of valid code units in @repl
 * @repl: corrected decomposition sequence (base letter + combining marks)
 */
struct hfsplus_legacy_decomp {
	u16 uc;
	u8 len;
#define HFSPLUS_LEGACY_DECOMP_MAX_LEN	(3)
	u16 repl[HFSPLUS_LEGACY_DECOMP_MAX_LEN];
};

/*
 * struct hfsplus_legacy_seq_fixup - corrected replacement for a short
 *                                   sequence of already-decomposed code units
 * @match_len: number of valid code units in @match
 * @match: decomposed sequence to look for in the buffer
 * @repl_len: number of valid code units in @repl
 * @repl: corrected sequence to substitute in place of @match
 */
struct hfsplus_legacy_seq_fixup {
	u8 match_len;
#define HFSPLUS_LEGACY_SEQ_MAX_MATCH	(3)
	u16 match[HFSPLUS_LEGACY_SEQ_MAX_MATCH];
	u8 repl_len;
#define HFSPLUS_LEGACY_SEQ_MAX_REPL	(2)
	u16 repl[HFSPLUS_LEGACY_SEQ_MAX_REPL];
};

extern u16 hfsplus_case_fold_table[];
extern u16 hfsplus_decompose_table[];
extern u16 hfsplus_compose_table[];
extern struct hfsplus_ccc_range hfsplus_ccc_table[];
extern struct hfsplus_legacy_decomp hfsplus_legacy_decomp_table[];
extern struct hfsplus_legacy_seq_fixup hfsplus_legacy_seq_fixups[];

/*
 * Look up the Unicode canonical combining class of a BMP code point.
 * Returns 0 ("Not_Reordered") for any code point not listed in the table,
 * which includes every ordinary base character.
 */
static inline
u8 hfsplus_combining_class(u16 c)
{
	int lo = 0, hi = ARRAY_SIZE(hfsplus_ccc_table) - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const struct hfsplus_ccc_range *r = &hfsplus_ccc_table[mid];

		if (c < r->first)
			hi = mid - 1;
		else if (c > r->last)
			lo = mid + 1;
		else
			return r->combining_class;
	}

	return 0;
}

/*
 * Look up the corrected canonical decomposition for a single BMP code
 * point that Apple Technote #1150's own decomposition table (above)
 * lacks. Returns NULL (and leaves *size alone) if @uc isn't one of these.
 */
static inline
const u16 *hfsplus_legacy_decompose(u16 uc, int *size)
{
	int lo = 0, hi = ARRAY_SIZE(hfsplus_legacy_decomp_table) - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		const struct hfsplus_legacy_decomp *e =
				&hfsplus_legacy_decomp_table[mid];

		if (uc < e->uc)
			hi = mid - 1;
		else if (uc > e->uc)
			lo = mid + 1;
		else {
			*size = e->len;
			return e->repl;
		}
	}

	return NULL;
}

/*
 * Scan an already decomposed and canonically-reordered code unit buffer
 * for any of the short legacy sequences above and replace them in place
 * with their corrected form. @len is updated to the buffer's new length.
 */
static inline
void hfsplus_fixup_legacy_sequences(u16 *buf, int *len)
{
	int i = 0;

	while (i < *len) {
		unsigned int j;
		bool matched = false;

		for (j = 0; j < ARRAY_SIZE(hfsplus_legacy_seq_fixups); j++) {
			const struct hfsplus_legacy_seq_fixup *f =
						&hfsplus_legacy_seq_fixups[j];
			u16 *src, *dst;
			size_t copy_len;
			int k;

			if (i + f->match_len > *len)
				continue;
			for (k = 0; k < f->match_len; k++)
				if (buf[i + k] != f->match[k])
					break;
			if (k != f->match_len)
				continue;

			dst = &buf[i + f->repl_len];
			src = &buf[i + f->match_len];
			copy_len = (*len - i - f->match_len) * sizeof(*buf);
			memmove(dst, src, copy_len);

			dst = &buf[i];
			src = f->repl;
			copy_len = f->repl_len * sizeof(*buf);
			memcpy(dst, src, copy_len);

			*len += f->repl_len - f->match_len;
			matched = true;
			break;
		}

		if (!matched)
			i++;
	}
}

#endif /* _LINUX_HFSPLUS_UNICODE_H */
