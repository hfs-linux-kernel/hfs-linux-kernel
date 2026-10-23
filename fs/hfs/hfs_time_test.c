// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for HFS date/time conversion
 *
 * Copyright (C) 2026 Viacheslav Dubeyko <slava@dubeyko.com>
 */

#include <kunit/test.h>
#include <linux/unaligned.h>
#include "hfs_fs.h"

/* January 1, 1970, 00:00:00 UTC */
#define TEST_UNIX_EPOCH_SECS		0LL
/* December 31, 1969, 23:59:59 UTC */
#define TEST_BEFORE_UNIX_EPOCH_SECS	-1LL
/* January 1, 2000, 00:00:00 UTC */
#define TEST_Y2000_SECS			946684800LL
/* January 19, 2038, 03:14:08 UTC (signed 32-bit overflow) */
#define TEST_Y2038_SECS			2147483648LL
/* January 1, 2026, 00:00:00 UTC */
#define TEST_Y2026_SECS			1767225600LL
/* January 1, 2100, 00:00:00 UTC */
#define TEST_Y2100_SECS			4102444800LL
/* January 1, 1900, 00:00:00 UTC */
#define TEST_Y1900_SECS			-2208988800LL

/* on-disk (1904-based) values of the timestamps above */
#define TEST_Y2000_MT			3029529600U
#define TEST_Y2026_MT			3850070400U
#define TEST_Y2038_MT			4230328448U
#define TEST_Y2100_EXT_MT		6185289600ULL

/* garbage of another implementation in the spare bytes */
#define TEST_FOREIGN_BYTE		0x5A

/*
 * HFS keeps timestamps in local time. The expected values
 * are shifted by the local time zone offset.
 */
static time64_t tz_secs(void)
{
	return sys_tz.tz_minuteswest * 60;
}

static u32 mt_raw(__be32 mt)
{
	return be32_to_cpu(mt);
}

static hfs_cat_rec *alloc_cat_rec(struct kunit *test, s8 type)
{
	hfs_cat_rec *rec;

	rec = kunit_kzalloc(test, sizeof(*rec), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, rec);
	rec->type = type;

	return rec;
}

static u64 file_ext_mt(hfs_cat_rec *rec)
{
	return get_unaligned_be64(&rec->file.FndrInfo);
}

static u64 dir_ext_mt(hfs_cat_rec *rec)
{
	return get_unaligned_be64(rec->dir.Resrv);
}

/* Test the time range constants */
static void hfs_time_constants_test(struct kunit *test)
{
	/* 32-bit unsigned on-disk field covers exactly U32_MAX seconds */
	KUNIT_EXPECT_EQ(test, (s64)U32_MAX,
			HFS_MAX_TIMESTAMP_SECS - HFS_MIN_TIMESTAMP_SECS);
	KUNIT_EXPECT_EQ(test, -(s64)HFS_UTC_OFFSET, HFS_MIN_TIMESTAMP_SECS);

	/* spare bytes extend the supported range beyond February 2040 */
	KUNIT_EXPECT_GT(test, HFS_EXT_MAX_TIMESTAMP_SECS,
			HFS_MAX_TIMESTAMP_SECS);

	/* the extended timestamp mark is the maximal on-disk value */
	KUNIT_EXPECT_EQ(test, U32_MAX, mt_raw(HFS_EXT_TIMESTAMP_MARK));
	KUNIT_EXPECT_EQ(test, 8, (int)HFS_EXT_TIMESTAMP_SIZE);

	/* the spare bytes do not overlap the used fields */
	KUNIT_EXPECT_GE(test, offsetof(struct hfs_fxinfo, fdUnused) + 6,
			HFS_EXT_TIMESTAMP_SIZE);
	KUNIT_EXPECT_GE(test, offsetof(struct hfs_fxinfo, fdComment),
			HFS_EXT_TIMESTAMP_SIZE + 2);
	KUNIT_EXPECT_GE(test, sizeof(((struct hfs_cat_dir *)0)->Resrv),
			HFS_EXT_TIMESTAMP_SIZE);
}

/* Test conversion of on-disk (1904-based) timestamp into 1970-based one */
static void hfs_m_to_utime_test(struct kunit *test)
{
	time64_t tz = tz_secs();

	KUNIT_EXPECT_EQ(test, HFS_MIN_TIMESTAMP_SECS + tz,
			__hfs_m_to_utime(cpu_to_be32(0)));
	KUNIT_EXPECT_EQ(test, TEST_UNIX_EPOCH_SECS + tz,
			__hfs_m_to_utime(cpu_to_be32(HFS_UTC_OFFSET)));
	KUNIT_EXPECT_EQ(test, TEST_BEFORE_UNIX_EPOCH_SECS + tz,
			__hfs_m_to_utime(cpu_to_be32(HFS_UTC_OFFSET - 1)));
	KUNIT_EXPECT_EQ(test, TEST_Y2000_SECS + tz,
			__hfs_m_to_utime(cpu_to_be32(TEST_Y2000_MT)));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS + tz,
			__hfs_m_to_utime(cpu_to_be32(TEST_Y2026_MT)));
	KUNIT_EXPECT_EQ(test, TEST_Y2038_SECS + tz,
			__hfs_m_to_utime(cpu_to_be32(TEST_Y2038_MT)));
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS + tz,
			__hfs_m_to_utime(HFS_EXT_TIMESTAMP_MARK));
}

/* Test conversion of 1970-based timestamp into on-disk (1904-based) one */
static void hfs_u_to_mtime_test(struct kunit *test)
{
	time64_t tz = tz_secs();

	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfs_u_to_mtime(HFS_MIN_TIMESTAMP_SECS + tz)));
	KUNIT_EXPECT_EQ(test, HFS_UTC_OFFSET,
			mt_raw(__hfs_u_to_mtime(TEST_UNIX_EPOCH_SECS + tz)));
	KUNIT_EXPECT_EQ(test, HFS_UTC_OFFSET - 1,
			mt_raw(__hfs_u_to_mtime(TEST_BEFORE_UNIX_EPOCH_SECS + tz)));
	KUNIT_EXPECT_EQ(test, TEST_Y2000_MT,
			mt_raw(__hfs_u_to_mtime(TEST_Y2000_SECS + tz)));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_MT,
			mt_raw(__hfs_u_to_mtime(TEST_Y2026_SECS + tz)));
	KUNIT_EXPECT_EQ(test, TEST_Y2038_MT,
			mt_raw(__hfs_u_to_mtime(TEST_Y2038_SECS + tz)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfs_u_to_mtime(HFS_MAX_TIMESTAMP_SECS + tz)));
}

/* Test that out of range timestamps are clamped instead of wrapped */
static void hfs_u_to_mtime_clamp_test(struct kunit *test)
{
	time64_t tz = tz_secs();

	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfs_u_to_mtime(TEST_Y1900_SECS + tz)));
	KUNIT_EXPECT_EQ(test, 0U,
			mt_raw(__hfs_u_to_mtime(HFS_MIN_TIMESTAMP_SECS + tz - 1)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfs_u_to_mtime(HFS_MAX_TIMESTAMP_SECS + tz + 1)));
	KUNIT_EXPECT_EQ(test, U32_MAX,
			mt_raw(__hfs_u_to_mtime(TEST_Y2100_SECS + tz)));
}

/* Test round trip of the timestamps from the supported range */
static void hfs_time_round_trip_test(struct kunit *test)
{
	static const time64_t values[] = {
		HFS_MIN_TIMESTAMP_SECS,
		TEST_BEFORE_UNIX_EPOCH_SECS,
		TEST_UNIX_EPOCH_SECS,
		TEST_Y2000_SECS,
		TEST_Y2026_SECS,
		TEST_Y2038_SECS,
		HFS_MAX_TIMESTAMP_SECS,
	};
	time64_t tz = tz_secs();
	int i;

	for (i = 0; i < ARRAY_SIZE(values); i++) {
		time64_t ut = values[i] + tz;

		KUNIT_EXPECT_EQ(test, ut,
				__hfs_m_to_utime(__hfs_u_to_mtime(ut)));
	}
}

/* Test conversion of the extended (64-bit) on-disk timestamps */
static void hfs_ext_mtime_test(struct kunit *test)
{
	time64_t tz = tz_secs();

	KUNIT_EXPECT_EQ(test, TEST_Y2100_EXT_MT,
			__hfs_u_to_ext_mtime(TEST_Y2100_SECS + tz));
	KUNIT_EXPECT_EQ(test, TEST_Y2100_SECS + tz,
			__hfs_ext_m_to_utime(TEST_Y2100_EXT_MT));

	/* the extended value is the continuation of the 32-bit one */
	KUNIT_EXPECT_EQ(test, (u64)TEST_Y2026_MT,
			__hfs_u_to_ext_mtime(TEST_Y2026_SECS + tz));
	KUNIT_EXPECT_EQ(test, (u64)U32_MAX + 1,
			__hfs_u_to_ext_mtime(HFS_MAX_TIMESTAMP_SECS + tz + 1));

	/* clamping */
	KUNIT_EXPECT_EQ(test, 0ULL, __hfs_u_to_ext_mtime(TIME64_MIN));
	KUNIT_EXPECT_LE(test, __hfs_u_to_ext_mtime(TIME64_MAX),
			HFS_EXT_MAX_MAC_DATE);
	KUNIT_EXPECT_TRUE(test,
		hfs_ext_mt_is_valid(__hfs_u_to_ext_mtime(TIME64_MAX)));
	KUNIT_EXPECT_EQ(test,
		HFS_EXT_MAX_TIMESTAMP_SECS,
		__hfs_ext_m_to_utime(__hfs_u_to_ext_mtime(TIME64_MAX)));
}

/* Test validation of the extended on-disk timestamps */
static void hfs_ext_mt_is_valid_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, hfs_ext_mt_is_valid(0));
	KUNIT_EXPECT_FALSE(test, hfs_ext_mt_is_valid(TEST_Y2026_MT));
	KUNIT_EXPECT_FALSE(test, hfs_ext_mt_is_valid(U32_MAX));
	KUNIT_EXPECT_TRUE(test, hfs_ext_mt_is_valid((u64)U32_MAX + 1));
	KUNIT_EXPECT_TRUE(test, hfs_ext_mt_is_valid(TEST_Y2100_EXT_MT));
	KUNIT_EXPECT_TRUE(test, hfs_ext_mt_is_valid(HFS_EXT_MAX_MAC_DATE));
	KUNIT_EXPECT_FALSE(test, hfs_ext_mt_is_valid(HFS_EXT_MAX_MAC_DATE + 1));
	KUNIT_EXPECT_FALSE(test, hfs_ext_mt_is_valid(U64_MAX));

	KUNIT_EXPECT_TRUE(test,
		hfs_mt_is_ext_timestamp(HFS_EXT_TIMESTAMP_MARK));
	KUNIT_EXPECT_FALSE(test,
		hfs_mt_is_ext_timestamp(cpu_to_be32(TEST_Y2026_MT)));
	KUNIT_EXPECT_FALSE(test, hfs_mt_is_ext_timestamp(cpu_to_be32(0)));
}

/* Test regular timestamp in the file record */
static void hfs_cat_file_regular_mtime_test(struct kunit *test)
{
	hfs_cat_rec *rec = alloc_cat_rec(test, HFS_CDR_FIL);
	time64_t tz = tz_secs();

	hfs_cat_write_mtime(rec, TEST_Y2026_SECS + tz);
	KUNIT_EXPECT_EQ(test, TEST_Y2026_MT, mt_raw(rec->file.MdDat));
	KUNIT_EXPECT_EQ(test, 0ULL, file_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS + tz, hfs_cat_read_mtime(rec));
}

/* Test timestamp after February 2040 in the file record */
static void hfs_cat_file_ext_mtime_test(struct kunit *test)
{
	hfs_cat_rec *rec = alloc_cat_rec(test, HFS_CDR_FIL);
	time64_t tz = tz_secs();

	rec->file.FlNum = cpu_to_be32(HFS_FIRSTUSER_CNID);
	rec->file.CrDat = cpu_to_be32(TEST_Y2000_MT);
	rec->file.FndrInfo.fdUnused[6] = TEST_FOREIGN_BYTE;
	rec->file.FndrInfo.fdUnused[7] = TEST_FOREIGN_BYTE;
	rec->file.FndrInfo.fdComment = cpu_to_be16(0x1234);
	rec->file.FndrInfo.fdPutAway = cpu_to_be32(0xDEADBEEF);

	hfs_cat_write_mtime(rec, TEST_Y2100_SECS + tz);
	KUNIT_EXPECT_TRUE(test, hfs_mt_is_ext_timestamp(rec->file.MdDat));
	KUNIT_EXPECT_EQ(test, TEST_Y2100_EXT_MT, file_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2100_SECS + tz, hfs_cat_read_mtime(rec));

	/* other fields are not touched */
	KUNIT_EXPECT_EQ(test, HFS_FIRSTUSER_CNID, be32_to_cpu(rec->file.FlNum));
	KUNIT_EXPECT_EQ(test, TEST_Y2000_MT, mt_raw(rec->file.CrDat));
	KUNIT_EXPECT_EQ(test, TEST_FOREIGN_BYTE,
			rec->file.FndrInfo.fdUnused[6]);
	KUNIT_EXPECT_EQ(test, TEST_FOREIGN_BYTE,
			rec->file.FndrInfo.fdUnused[7]);
	KUNIT_EXPECT_EQ(test, 0x1234, be16_to_cpu(rec->file.FndrInfo.fdComment));
	KUNIT_EXPECT_EQ(test, 0xDEADBEEF,
			be32_to_cpu(rec->file.FndrInfo.fdPutAway));
	KUNIT_EXPECT_EQ(test, 0U, rec->file.Resrv);

	/* timestamp becomes representable again: stale value is removed */
	hfs_cat_write_mtime(rec, TEST_Y2026_SECS + tz);
	KUNIT_EXPECT_EQ(test, TEST_Y2026_MT, mt_raw(rec->file.MdDat));
	KUNIT_EXPECT_EQ(test, 0ULL, file_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS + tz, hfs_cat_read_mtime(rec));
	KUNIT_EXPECT_EQ(test, TEST_FOREIGN_BYTE,
			rec->file.FndrInfo.fdUnused[6]);
	KUNIT_EXPECT_EQ(test, 0x1234, be16_to_cpu(rec->file.FndrInfo.fdComment));
}

/* Test timestamp after February 2040 in the directory record */
static void hfs_cat_dir_ext_mtime_test(struct kunit *test)
{
	hfs_cat_rec *rec = alloc_cat_rec(test, HFS_CDR_DIR);
	time64_t tz = tz_secs();
	int i;

	rec->dir.DirID = cpu_to_be32(HFS_FIRSTUSER_CNID);
	rec->dir.FndrInfo.frPutAway = cpu_to_be32(0xDEADBEEF);
	for (i = HFS_EXT_TIMESTAMP_SIZE; i < sizeof(rec->dir.Resrv); i++)
		rec->dir.Resrv[i] = TEST_FOREIGN_BYTE;

	hfs_cat_write_mtime(rec, TEST_Y2026_SECS + tz);
	KUNIT_EXPECT_EQ(test, TEST_Y2026_MT, mt_raw(rec->dir.MdDat));
	KUNIT_EXPECT_EQ(test, 0ULL, dir_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS + tz, hfs_cat_read_mtime(rec));

	hfs_cat_write_mtime(rec, TEST_Y2100_SECS + tz);
	KUNIT_EXPECT_TRUE(test, hfs_mt_is_ext_timestamp(rec->dir.MdDat));
	KUNIT_EXPECT_EQ(test, TEST_Y2100_EXT_MT, dir_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2100_SECS + tz, hfs_cat_read_mtime(rec));

	/* other fields are not touched */
	KUNIT_EXPECT_EQ(test, HFS_FIRSTUSER_CNID, be32_to_cpu(rec->dir.DirID));
	KUNIT_EXPECT_EQ(test, 0xDEADBEEF,
			be32_to_cpu(rec->dir.FndrInfo.frPutAway));
	for (i = HFS_EXT_TIMESTAMP_SIZE; i < sizeof(rec->dir.Resrv); i++)
		KUNIT_EXPECT_EQ(test, TEST_FOREIGN_BYTE, rec->dir.Resrv[i]);

	hfs_cat_write_mtime(rec, TEST_Y2000_SECS + tz);
	KUNIT_EXPECT_EQ(test, TEST_Y2000_MT, mt_raw(rec->dir.MdDat));
	KUNIT_EXPECT_EQ(test, 0ULL, dir_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2000_SECS + tz, hfs_cat_read_mtime(rec));
}

/* Test that another implementation updating MdDat invalidates the value */
static void hfs_cat_foreign_mtime_update_test(struct kunit *test)
{
	hfs_cat_rec *rec = alloc_cat_rec(test, HFS_CDR_FIL);
	time64_t tz = tz_secs();

	hfs_cat_write_mtime(rec, TEST_Y2100_SECS + tz);
	KUNIT_EXPECT_TRUE(test, hfs_mt_is_ext_timestamp(rec->file.MdDat));

	/* Mac OS updates MdDat but keeps the spare bytes */
	rec->file.MdDat = cpu_to_be32(TEST_Y2026_MT);
	KUNIT_EXPECT_EQ(test, TEST_Y2100_EXT_MT, file_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS + tz, hfs_cat_read_mtime(rec));
}

/* Test the mark without the valid extended value */
static void hfs_cat_mark_without_ext_test(struct kunit *test)
{
	hfs_cat_rec *rec = alloc_cat_rec(test, HFS_CDR_FIL);
	time64_t tz = tz_secs();

	/* the spare bytes are zero */
	rec->file.MdDat = HFS_EXT_TIMESTAMP_MARK;
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS + tz,
			hfs_cat_read_mtime(rec));

	/* the value is representable by MdDat */
	put_unaligned_be64(TEST_Y2026_MT, &rec->file.FndrInfo);
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS + tz,
			hfs_cat_read_mtime(rec));

	/* the value cannot be converted into time64_t */
	put_unaligned_be64(U64_MAX, &rec->file.FndrInfo);
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS + tz,
			hfs_cat_read_mtime(rec));
}

/* Test that the spare bytes of another implementation are preserved */
static void hfs_cat_foreign_spare_bytes_test(struct kunit *test)
{
	hfs_cat_rec *rec = alloc_cat_rec(test, HFS_CDR_FIL);
	time64_t tz = tz_secs();
	u64 foreign;

	memset(&rec->file.FndrInfo, TEST_FOREIGN_BYTE, HFS_EXT_TIMESTAMP_SIZE);
	foreign = file_ext_mt(rec);
	rec->file.MdDat = cpu_to_be32(TEST_Y2000_MT);

	hfs_cat_write_mtime(rec, TEST_Y2026_SECS + tz);
	KUNIT_EXPECT_EQ(test, TEST_Y2026_MT, mt_raw(rec->file.MdDat));
	KUNIT_EXPECT_EQ(test, foreign, file_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, TEST_Y2026_SECS + tz, hfs_cat_read_mtime(rec));

	/*
	 * The maximal regular value is equal to the mark. The spare bytes
	 * must be cleared, otherwise they will be treated as the extended
	 * value.
	 */
	hfs_cat_write_mtime(rec, HFS_MAX_TIMESTAMP_SECS + tz);
	KUNIT_EXPECT_TRUE(test, hfs_mt_is_ext_timestamp(rec->file.MdDat));
	KUNIT_EXPECT_EQ(test, 0ULL, file_ext_mt(rec));
	KUNIT_EXPECT_EQ(test, HFS_MAX_TIMESTAMP_SECS + tz,
			hfs_cat_read_mtime(rec));
}

static struct kunit_case hfs_time_test_cases[] = {
	KUNIT_CASE(hfs_time_constants_test),
	KUNIT_CASE(hfs_m_to_utime_test),
	KUNIT_CASE(hfs_u_to_mtime_test),
	KUNIT_CASE(hfs_u_to_mtime_clamp_test),
	KUNIT_CASE(hfs_time_round_trip_test),
	KUNIT_CASE(hfs_ext_mtime_test),
	KUNIT_CASE(hfs_ext_mt_is_valid_test),
	KUNIT_CASE(hfs_cat_file_regular_mtime_test),
	KUNIT_CASE(hfs_cat_file_ext_mtime_test),
	KUNIT_CASE(hfs_cat_dir_ext_mtime_test),
	KUNIT_CASE(hfs_cat_foreign_mtime_update_test),
	KUNIT_CASE(hfs_cat_mark_without_ext_test),
	KUNIT_CASE(hfs_cat_foreign_spare_bytes_test),
	{}
};

static struct kunit_suite hfs_time_test_suite = {
	.name = "hfs_time",
	.test_cases = hfs_time_test_cases,
};

kunit_test_suite(hfs_time_test_suite);

MODULE_DESCRIPTION("KUnit tests for HFS date/time conversion");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
