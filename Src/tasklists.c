/*
 * Task Lists - Palm OS 3.0+ (68k)
 *
 * Three recurring checklists in one database: Daily, Weekly and Monthly.
 * Daily items uncheck themselves every day.  Weekly and monthly items show
 * their next due date (the last day of the current week / month); when
 * checked they show the date they were completed, and they uncheck when
 * the next week / month starts.  Weeks start on a configurable day
 * (Monday by default).
 *
 * Data model (one database, one record per item; same layout as version 1):
 *   bytes 0-3  UInt32  stamp  (local day number: seconds since 1904 / 86400)
 *                      daily:           day the item was last checked/reset
 *                      weekly/monthly:  day the item was completed
 *   byte  4    UInt8   done   (0 or 1)
 *   byte  5    UInt8   kind   (0 daily, 1 weekly, 2 monthly)
 *                      version 1 stored 0 here, so old items are daily
 *   byte  6..  Char[]  text, NUL-terminated
 *   optional   UInt32  custom due day, stored right after the text's NUL.
 *                      Absent (record ends at the NUL) = automatic due date.
 *                      Only used by weekly/monthly items; cleared when the
 *                      completed item resets at the next week / month.
 *
 * Beam payload (extension .jdl, creator JsDL):
 *   byte 0  UInt8  format version (1)
 *   byte 1  UInt8  list kind
 *   byte 2  UInt8  item count (1..200)
 *   then    count NUL-terminated item texts
 * Received items are added unchecked.
 *
 * Careful: ReceiveBeam() and RegisterBeaming() run from launch codes where
 * the application globals may not exist.  They (and every helper they
 * call) use no global variables and no string literals.
 */

#include <PalmOS.h>
#include "tasklists_res.h"

#define kCreator        'JsDL'
#define kDBType         'Data'
#define kDBName         "DailyChecklistDB"
#define kDBNameLen      16

#define kHdrSize        6
#define kMaxText        40
#define kMaxItems       200             /* per list */
#define kRowsOnScreen   10
#define kSecsPerDay     86400UL
#define kNewItem        0xFFFF

#define kKindDaily      0
#define kKindWeekly     1
#define kKindMonthly    2
#define kNumKinds       3

#define kBeamVersion    1
#define kBeamMax        (3 + kMaxItems * (kMaxText + 1))

#define kMinROMVersion  sysMakeROMVersion(3, 0, 0, sysROMStageRelease, 0)
#define kROM32          sysMakeROMVersion(3, 2, 0, sysROMStageRelease, 0)

typedef struct {
    UInt8 weekStart;                    /* 0 = Sunday .. 6 = Saturday */
    UInt8 kind;                         /* list shown last time */
} AppPrefs;

static DmOpenRef gDB        = 0;
static UInt16    gTopIndex  = 0;        /* position in gIdx shown in row 0 */
static UInt32    gToday     = 0;        /* day number of the last reset pass */
static UInt8     gKind      = kKindDaily;
static UInt8     gWeekStart = 1;        /* Monday */
static UInt16    gIdx[kMaxItems];       /* record indexes of the current list */
static UInt16    gCount     = 0;
static UInt32    gEditDue   = 0;        /* custom due day in the edit dialog, 0 = auto */
static Char      gDueLabel[16];         /* label of the due-date selector */

static const Char *kEditTitles[kNumKinds] = {
    "Daily Task", "Weekly Task", "Monthly Task"
};
static const Char *kBeamNames[kNumKinds] = {
    "Daily list", "Weekly list", "Monthly list"
};

/* ------------------------------------------------------------------ */
/* Helpers (no globals - also used from the beam receive path)         */
/* ------------------------------------------------------------------ */

static UInt32 TodayDays(void)
{
    return TimGetSeconds() / kSecsPerDay;
}

static Err RomVersionCompatible(UInt32 required, UInt16 launchFlags)
{
    UInt32 romVersion = 0;

    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &romVersion);
    if (romVersion < required) {
        if ((launchFlags & (sysAppLaunchFlagNewGlobals | sysAppLaunchFlagUIApp)) ==
            (sysAppLaunchFlagNewGlobals | sysAppLaunchFlagUIApp)) {
            FrmAlert(RomIncompatibleAlert);
        }
        return sysErrRomIncompatible;
    }
    return errNone;
}

/* Reads the fixed part of a record.  Returns false if it is missing or
 * too short to be one of ours. */
static Boolean ReadHeader(DmOpenRef db, UInt16 index, UInt32 *stamp,
                          UInt8 *done, UInt8 *kind)
{
    MemHandle h = DmQueryRecord(db, index);
    UInt8    *p;

    if (!h || MemHandleSize(h) <= kHdrSize)
        return false;
    p = (UInt8 *)MemHandleLock(h);
    MemMove(stamp, p, 4);
    *done = p[4];
    *kind = (p[5] < kNumKinds) ? p[5] : kKindDaily;
    MemHandleUnlock(h);
    return true;
}

static UInt16 CountKind(DmOpenRef db, UInt8 kind)
{
    UInt16 i, n = DmNumRecords(db), count = 0;
    UInt32 stamp;
    UInt8  done, k;

    for (i = 0; i < n; i++)
        if (ReadHeader(db, i, &stamp, &done, &k) && k == kind)
            count++;
    return count;
}

/* Custom due day stored after the text, or 0 for the automatic date. */
static UInt32 ReadDue(DmOpenRef db, UInt16 index)
{
    MemHandle h = DmQueryRecord(db, index);
    UInt8    *p;
    UInt32    size, end, due = 0;

    if (!h)
        return 0;
    size = MemHandleSize(h);
    p = (UInt8 *)MemHandleLock(h);
    if (size > kHdrSize) {
        end = kHdrSize + StrLen((Char *)p + kHdrSize) + 1;
        if (size >= end + 4)
            MemMove(&due, p + end, 4);
    }
    MemHandleUnlock(h);
    return due;
}

/* Appends an unchecked item to the database.  due = 0 means automatic. */
static Boolean AddRecord(DmOpenRef db, UInt8 kind, const Char *text, UInt32 due)
{
    UInt16    index = dmMaxRecordIndex;     /* append at the end */
    UInt16    len   = StrLen(text) + 1;
    UInt32    today = TodayDays();
    UInt8     flags[2];
    MemHandle h;
    MemPtr    p;

    flags[0] = 0;                           /* done */
    flags[1] = kind;

    h = DmNewRecord(db, &index, kHdrSize + len + (due ? 4 : 0));
    if (!h)
        return false;

    p = MemHandleLock(h);
    DmWrite(p, 0, &today, 4);
    DmWrite(p, 4, flags, 2);
    DmWrite(p, kHdrSize, text, len);
    if (due)
        DmWrite(p, kHdrSize + len, &due, 4);
    MemHandleUnlock(h);
    DmReleaseRecord(db, index, true);
    return true;
}

static void CapitalizeFirst(Char *s)
{
    if (s[0] >= 'a' && s[0] <= 'z')
        s[0] = (Char)(s[0] - ('a' - 'A'));
}

/* ------------------------------------------------------------------ */
/* Dates                                                               */
/* ------------------------------------------------------------------ */

/* Day numbers count from 1904-01-01, which was a Friday. 0 = Sunday. */
static UInt8 DayOfWeekOf(UInt32 days)
{
    return (UInt8)((days + 5) % 7);
}

static void MonthBounds(UInt32 days, UInt32 *first, UInt32 *last)
{
    DateTimeType dt;

    TimSecondsToDateTime(days * kSecsPerDay, &dt);
    dt.hour = 0;
    dt.minute = 0;
    dt.second = 0;
    dt.day = 1;
    *first = TimDateTimeToSeconds(&dt) / kSecsPerDay;
    dt.day = (Int16)DaysInMonth(dt.month, dt.year);
    *last = TimDateTimeToSeconds(&dt) / kSecsPerDay;
}

/* First day of the week / month that contains 'today'. */
static UInt32 PeriodStart(UInt8 kind, UInt32 today)
{
    UInt32 first, last;

    if (kind == kKindWeekly) {
        UInt8 back = (UInt8)((DayOfWeekOf(today) + 7 - gWeekStart) % 7);
        return today - back;
    }
    if (kind == kKindMonthly) {
        MonthBounds(today, &first, &last);
        return first;
    }
    return today;
}

/* Last day of the week / month that contains 'today' (the due date). */
static UInt32 PeriodEnd(UInt8 kind, UInt32 today)
{
    UInt32 first, last;

    if (kind == kKindWeekly)
        return PeriodStart(kind, today) + 6;
    if (kind == kKindMonthly) {
        MonthBounds(today, &first, &last);
        return last;
    }
    return today;
}

/* "M/D" or "D/M" depending on the device's date format; out >= 8 bytes. */
static void FormatDay(UInt32 days, Char *out)
{
    DateTimeType dt;
    Char         num[8];
    Boolean      dayFirst = false;

    TimSecondsToDateTime(days * kSecsPerDay, &dt);
    switch ((UInt16)PrefGetPreference(prefDateFormat)) {
    case dfDMYWithSlashes:
    case dfDMYWithDots:
    case dfDMYWithDashes:
    case dfDMYLong:
        dayFirst = true;
        break;
    default:
        break;
    }

    StrIToA(out, dayFirst ? dt.day : dt.month);
    StrCat(out, "/");
    StrIToA(num, dayFirst ? dt.month : dt.day);
    StrCat(out, num);
}

/* ------------------------------------------------------------------ */
/* Preferences                                                         */
/* ------------------------------------------------------------------ */

static void LoadPrefs(void)
{
    AppPrefs prefs;
    UInt16   size = sizeof(prefs);
    Int16    ver  = PrefGetAppPreferences(kCreator, 0, &prefs, &size, true);

    if (ver != noPreferenceFound && size == sizeof(prefs) &&
        prefs.weekStart < 7 && prefs.kind < kNumKinds) {
        gWeekStart = prefs.weekStart;
        gKind      = prefs.kind;
    }
}

static void SavePrefs(void)
{
    AppPrefs prefs;

    prefs.weekStart = gWeekStart;
    prefs.kind      = gKind;
    PrefSetAppPreferences(kCreator, 0, 1, &prefs, sizeof(prefs), true);
}

/* ------------------------------------------------------------------ */
/* Database                                                            */
/* ------------------------------------------------------------------ */

/* Creates the (empty) database.  Used at launch and when a beam arrives
 * before the app has ever been run, so no globals or string literals. */
static Err CreateDatabase(void)
{
    Char    name[kDBNameLen + 1];
    LocalID dbID;
    UInt16  attr = 0;
    Err     err;

    /* "DailyChecklistDB" - the name is kept from version 1 */
    name[0]  = 'D'; name[1]  = 'a'; name[2]  = 'i'; name[3]  = 'l';
    name[4]  = 'y'; name[5]  = 'C'; name[6]  = 'h'; name[7]  = 'e';
    name[8]  = 'c'; name[9]  = 'k'; name[10] = 'l'; name[11] = 'i';
    name[12] = 's'; name[13] = 't'; name[14] = 'D'; name[15] = 'B';
    name[16] = 0;

    err = DmCreateDatabase(0, name, kCreator, kDBType, false);
    if (err)
        return err;

    /* Set the backup bit so HotSync backs the lists up. */
    dbID = DmFindDatabase(0, name);
    if (dbID) {
        DmDatabaseInfo(0, dbID, NULL, &attr, NULL, NULL, NULL, NULL,
                       NULL, NULL, NULL, NULL, NULL);
        attr |= dmHdrAttrBackup;
        DmSetDatabaseInfo(0, dbID, NULL, &attr, NULL, NULL, NULL, NULL,
                          NULL, NULL, NULL, NULL, NULL);
    }
    return errNone;
}

static Err OpenDatabase(void)
{
    Err err;

    gDB = DmOpenDatabaseByTypeCreator(kDBType, kCreator, dmModeReadWrite);
    if (gDB)
        return errNone;

    err = CreateDatabase();
    if (err)
        return err;

    gDB = DmOpenDatabaseByTypeCreator(kDBType, kCreator, dmModeReadWrite);
    if (!gDB)
        return DmGetLastErr();
    return errNone;
}

/* Stamps and sets/clears the done flag of one record. */
static void WriteHeader(UInt16 index, UInt32 stamp, UInt8 done)
{
    MemHandle h = DmGetRecord(gDB, index);
    MemPtr    p;

    if (!h)
        return;
    p = MemHandleLock(h);
    DmWrite(p, 0, &stamp, 4);
    DmWrite(p, 4, &done, 1);
    MemHandleUnlock(h);
    DmReleaseRecord(gDB, index, true);
}

static void SetDone(UInt16 index, Boolean done)
{
    WriteHeader(index, TodayDays(), done ? 1 : 0);
}

/* Replaces the text and the custom due day (0 = automatic). */
static Boolean SetItemText(UInt16 index, const Char *text, UInt32 due)
{
    UInt16    len = StrLen(text) + 1;
    MemHandle h;
    MemPtr    p;

    /* Resize first (record not checked out), then check out and write. */
    h = DmResizeRecord(gDB, index, kHdrSize + len + (due ? 4 : 0));
    if (!h)
        return false;

    h = DmGetRecord(gDB, index);
    if (!h)
        return false;
    p = MemHandleLock(h);
    DmWrite(p, kHdrSize, text, len);
    if (due)
        DmWrite(p, kHdrSize + len, &due, 4);
    MemHandleUnlock(h);
    DmReleaseRecord(gDB, index, true);
    return true;
}

/* Drops the custom due day so the automatic date applies again. */
static void ClearDue(UInt16 index)
{
    MemHandle h = DmQueryRecord(gDB, index);
    UInt32    size, end;
    MemPtr    p;

    if (!h || ReadDue(gDB, index) == 0)
        return;
    size = MemHandleSize(h);
    p = MemHandleLock(h);
    end = kHdrSize + StrLen((Char *)p + kHdrSize) + 1;
    MemHandleUnlock(h);
    if (size > end)
        DmResizeRecord(gDB, index, end);
}

/*
 * Daily items: reset when the stamp is not today.
 * Weekly / monthly items: a completed item resets once its completion day
 * is before the start of the current week / month.
 */
static void ApplyReset(void)
{
    UInt32 today   = TodayDays();
    UInt32 weekBeg = PeriodStart(kKindWeekly, today);
    UInt32 monBeg  = PeriodStart(kKindMonthly, today);
    UInt16 i, n = DmNumRecords(gDB);

    for (i = 0; i < n; i++) {
        UInt32 stamp;
        UInt8  done, kind;

        if (!ReadHeader(gDB, i, &stamp, &done, &kind))
            continue;
        if (kind == kKindDaily) {
            if (stamp != today)
                WriteHeader(i, today, 0);
        } else if (done) {
            UInt32 begin = (kind == kKindWeekly) ? weekBeg : monBeg;

            if (stamp < begin) {
                WriteHeader(i, stamp, 0);
                ClearDue(i);            /* next period: automatic due date */
            }
        }
    }
    gToday = today;
}

/* Rebuilds gIdx: the record indexes that belong to the current list. */
static void RebuildIndex(void)
{
    UInt16 i, n = DmNumRecords(gDB);
    UInt32 stamp;
    UInt8  done, kind;

    gCount = 0;
    for (i = 0; i < n && gCount < kMaxItems; i++)
        if (ReadHeader(gDB, i, &stamp, &done, &kind) && kind == gKind)
            gIdx[gCount++] = i;
}

static void UncheckAll(void)
{
    UInt16 i;
    UInt32 stamp;
    UInt8  done, kind;

    for (i = 0; i < gCount; i++)
        if (ReadHeader(gDB, gIdx[i], &stamp, &done, &kind) && done)
            SetDone(gIdx[i], false);
}

/* ------------------------------------------------------------------ */
/* Beaming (Exchange Manager)                                          */
/* ------------------------------------------------------------------ */

/* The Exchange Manager came with Palm OS 3.2 (some 3.1 ROMs such as the
 * Visor's have it too), so test for the trap instead of the version. */
static Boolean BeamAvailable(void)
{
    UInt32 romVer = 0;

    FtrGet(sysFtrCreator, sysFtrNumROMVersion, &romVer);
    if (romVer >= kROM32)
        return true;
    if (romVer < kMinROMVersion)
        return false;
    return SysGetTrapAddress(sysTrapExgPut) !=
           SysGetTrapAddress(sysTrapSysUnimplemented);
}

static void RegisterBeaming(void)
{
    Char ext[4];

    if (!BeamAvailable())
        return;
    ext[0] = 'j'; ext[1] = 'd'; ext[2] = 'l'; ext[3] = 0;
    ExgRegisterData(kCreator, exgRegExtensionID, ext);
}

/* Beams the given records (all of one list kind) as one payload. */
static void BeamItems(UInt8 kind, const UInt16 *idxs, UInt16 n)
{
    ExgSocketType s;
    Err           err;
    UInt8        *buf;
    UInt32        size = 3, off, sent;
    UInt16        i, count = 0;

    if (n == 0)
        return;
    if (!BeamAvailable()) {
        FrmAlert(NoBeamAlert);
        return;
    }
    buf = (UInt8 *)MemPtrNew(kBeamMax);
    if (!buf) {
        FrmAlert(BeamFailAlert);
        return;
    }

    for (i = 0; i < n && count < kMaxItems; i++) {
        MemHandle h = DmQueryRecord(gDB, idxs[i]);
        UInt16    len;
        Char     *t;

        if (!h || MemHandleSize(h) <= kHdrSize)
            continue;
        t = (Char *)MemHandleLock(h) + kHdrSize;
        len = StrLen(t);
        if (len > kMaxText)
            len = kMaxText;
        MemMove(buf + size, t, len);
        buf[size + len] = 0;
        size += len + 1;
        count++;
        MemHandleUnlock(h);
    }
    buf[0] = kBeamVersion;
    buf[1] = kind;
    buf[2] = (UInt8)count;

    if (count == 0) {
        MemPtrFree(buf);
        return;
    }

    MemSet(&s, sizeof(s), 0);
    s.description = (count == 1) ? (Char *)(buf + 3) : (Char *)kBeamNames[kind];
    s.name = "tasks.jdl";
    s.target = kCreator;
    s.length = size;

    err = ExgPut(&s);
    if (err == errNone) {
        off = 0;
        while (err == errNone && off < size) {
            sent = ExgSend(&s, buf + off, size - off, &err);
            if (sent == 0 && err == errNone)
                err = exgErrBadData;
            off += sent;
        }
        err = ExgDisconnect(&s, err);
    }
    MemPtrFree(buf);

    if (err != errNone && err != exgErrUserCancel)
        FrmAlert(BeamFailAlert);
}

/* Checks a received payload completely before anything is stored. */
static Boolean ValidBeam(const UInt8 *b, UInt32 total)
{
    UInt32 off = 3, start;
    UInt8  i;

    if (total < 4 || b[0] != kBeamVersion || b[1] >= kNumKinds ||
        b[2] == 0 || b[2] > kMaxItems)
        return false;
    for (i = 0; i < b[2]; i++) {
        start = off;
        while (off < total && b[off] != 0)
            off++;
        if (off >= total)                   /* no terminator */
            return false;
        if (off == start || off - start > kMaxText)
            return false;                   /* empty or too long */
        off++;
    }
    return off == total;
}

/*
 * Receives a beamed payload and adds its items.  Uses no globals: pass
 * db = 0 and it opens (or creates) the database itself, or pass the
 * running app's open database.  Returns the list kind in *kindOut.
 */
static Err ReceiveBeam(ExgSocketPtr s, DmOpenRef db, UInt8 *kindOut)
{
    Err     err;
    UInt8  *buf;
    UInt32  total = 0, n, off;
    Boolean own = false;

    *kindOut = 0xFF;
    err = ExgAccept(s);
    if (err != errNone)
        return err;

    buf = (UInt8 *)MemPtrNew(kBeamMax + 1);
    if (!buf) {
        ExgDisconnect(s, exgMemError);
        return exgMemError;
    }

    while (err == errNone) {
        n = ExgReceive(s, buf + total, kBeamMax + 1 - total, &err);
        if (err != errNone || n == 0)
            break;
        total += n;
        if (total > kBeamMax)
            err = exgErrBadData;
    }

    if (err == errNone && !ValidBeam(buf, total))
        err = exgErrBadData;

    if (err == errNone) {
        if (!db) {
            db = DmOpenDatabaseByTypeCreator(kDBType, kCreator, dmModeReadWrite);
            if (!db && CreateDatabase() == errNone)
                db = DmOpenDatabaseByTypeCreator(kDBType, kCreator, dmModeReadWrite);
            own = true;
        }
        if (!db) {
            err = exgErrAppError;
        } else {
            UInt8  kind  = buf[1];
            UInt16 have  = CountKind(db, kind);
            UInt16 added = 0, i;

            off = 3;
            for (i = 0; i < buf[2]; i++) {
                Char *t = (Char *)(buf + off);

                off += StrLen(t) + 1;
                if (have >= kMaxItems)
                    break;
                if (!AddRecord(db, kind, t, 0))
                    break;
                have++;
                added++;
            }
            if (added)
                *kindOut = kind;
            else
                err = exgMemError;
            if (own)
                DmCloseDatabase(db);
        }
    }

    MemPtrFree(buf);
    ExgDisconnect(s, err);
    return err;
}

/* ------------------------------------------------------------------ */
/* Table drawing                                                       */
/* ------------------------------------------------------------------ */

/* Custom draw callback for column 1: item text, plus the due / completed
 * date at the right edge for weekly and monthly items. */
static void DrawItemText(void *tableP, Int16 row, Int16 column,
                         RectangleType *bounds)
{
    UInt16    index = TblGetRowID((TablePtr)tableP, row);
    MemHandle h;
    UInt32    stamp;
    UInt8     done, kind;
    UInt8    *p;
    Char     *s;
    Char      date[24];
    Int16     width, len, dateW = 0;
    Boolean   fits;
    FontID    oldFont;

    if (!ReadHeader(gDB, index, &stamp, &done, &kind))
        return;
    h = DmQueryRecord(gDB, index);
    if (!h)
        return;

    WinEraseRectangle(bounds, 0);
    oldFont = FntSetFont(stdFont);
    width = bounds->extent.x - 1;

    date[0] = 0;
    if (kind != kKindDaily) {
        StrCopy(date, done ? "Done " : "Due ");
        if (done)
            FormatDay(stamp, date + StrLen(date));
        else {
            UInt32 due = ReadDue(gDB, index);

            FormatDay(due ? due : PeriodEnd(kind, TodayDays()),
                      date + StrLen(date));
        }
        dateW = FntCharsWidth(date, StrLen(date));
        width -= dateW + 4;
    }

    p   = (UInt8 *)MemHandleLock(h);
    s   = (Char *)(p + kHdrSize);
    len = StrLen(s);
    FntCharsInWidth(s, &width, &len, &fits);     /* trims len/width to fit */
    if (!fits) {
        /* Too long: leave room for "..." and trim again. */
        Int16 ellW = FntCharsWidth("...", 3);

        width = bounds->extent.x - 1 - (dateW ? dateW + 4 : 0) - ellW;
        len   = StrLen(s);
        FntCharsInWidth(s, &width, &len, &fits);
        WinDrawChars(s, len, bounds->topLeft.x + 1, bounds->topLeft.y);
        WinDrawChars("...", 3, bounds->topLeft.x + 1 + width,
                     bounds->topLeft.y);
        width += ellW;                           /* strike through it too */
    } else {
        WinDrawChars(s, len, bounds->topLeft.x + 1, bounds->topLeft.y);
    }

    if (done) {                                  /* strike through the text */
        Int16 y = bounds->topLeft.y + (FntCharHeight() / 2);
        WinDrawLine(bounds->topLeft.x + 1, y,
                    bounds->topLeft.x + 1 + width, y);
    }
    MemHandleUnlock(h);

    if (dateW)
        WinDrawChars(date, StrLen(date),
                     bounds->topLeft.x + bounds->extent.x - dateW - 1,
                     bounds->topLeft.y);
    FntSetFont(oldFont);
}

/* "3/7 done" at the bottom right. */
static void DrawCount(void)
{
    RectangleType r;
    Char          buf[24], num[8];
    UInt16        i, done = 0;
    UInt32        stamp;
    UInt8         d, k;
    Int16         w;
    FontID        oldFont;

    for (i = 0; i < gCount; i++)
        if (ReadHeader(gDB, gIdx[i], &stamp, &d, &k) && d)
            done++;

    StrIToA(buf, done);
    StrCat(buf, "/");
    StrIToA(num, gCount);
    StrCat(buf, num);
    StrCat(buf, " done");

    RctSetRectangle(&r, 50, 145, 108, 12);
    WinEraseRectangle(&r, 0);
    oldFont = FntSetFont(stdFont);
    w = FntCharsWidth(buf, StrLen(buf));
    WinDrawChars(buf, StrLen(buf), 158 - w, 145);
    FntSetFont(oldFont);
}

static void LoadTable(void)
{
    FormPtr      frm = FrmGetActiveForm();
    TablePtr     tbl = (TablePtr)FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, MainTable));
    ScrollBarPtr bar = (ScrollBarPtr)FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, MainScrollBar));
    UInt16       rows = TblGetNumberOfRows(tbl);
    UInt16       maxTop, r;

    RebuildIndex();
    maxTop = (gCount > rows) ? (gCount - rows) : 0;
    if (gTopIndex > maxTop)
        gTopIndex = maxTop;

    TblSetColumnUsable(tbl, 0, true);
    TblSetColumnUsable(tbl, 1, true);
    TblSetCustomDrawProcedure(tbl, 1, DrawItemText);

    for (r = 0; r < rows; r++) {
        UInt16 pos = gTopIndex + r;

        if (pos < gCount) {
            UInt32 stamp;
            UInt8  done = 0, kind;

            ReadHeader(gDB, gIdx[pos], &stamp, &done, &kind);
            TblSetRowUsable(tbl, r, true);
            TblSetRowSelectable(tbl, r, true);
            TblSetRowHeight(tbl, r, 11);
            TblSetRowID(tbl, r, gIdx[pos]);
            TblSetItemStyle(tbl, r, 0, checkboxTableItem);
            TblSetItemInt(tbl, r, 0, done);
            TblSetItemStyle(tbl, r, 1, customTableItem);
        } else {
            TblSetRowUsable(tbl, r, false);
        }
    }

    TblEraseTable(tbl);
    TblDrawTable(tbl);
    SclSetScrollBar(bar, gTopIndex, 0, maxTop, rows);
    DrawCount();
}

static void ScrollPage(Int16 dir)
{
    Int32 t = (Int32)gTopIndex + (Int32)dir * kRowsOnScreen;

    if (t < 0)
        t = 0;
    gTopIndex = (UInt16)t;      /* upper bound is clamped in LoadTable */
    LoadTable();
}

/* Pushes the tab button for the current list. */
static void SyncTabs(FormPtr frm)
{
    UInt8 k;

    for (k = 0; k < kNumKinds; k++)
        CtlSetValue((ControlPtr)FrmGetObjectPtr(frm,
                    FrmGetObjectIndex(frm, MainTabDaily + k)), k == gKind);
}

/* ------------------------------------------------------------------ */
/* Add / edit / delete dialog                                          */
/* ------------------------------------------------------------------ */

/* Shows "Due: 11/15" (custom date) or "Due: Auto" on the selector. */
static void SetDueLabel(FormPtr frm)
{
    StrCopy(gDueLabel, "Due: ");
    if (gEditDue)
        FormatDay(gEditDue, gDueLabel + StrLen(gDueLabel));
    else
        StrCat(gDueLabel, "Auto");
    CtlSetLabel((ControlPtr)FrmGetObjectPtr(frm,
                FrmGetObjectIndex(frm, EditDue)), gDueLabel);
}

/* Date picker for the custom due date. */
static void PickDue(void)
{
    DateTimeType dt;
    Int16        month, day, year;
    UInt32       autoDue = PeriodEnd(gKind, TodayDays());

    TimSecondsToDateTime((gEditDue ? gEditDue : autoDue) * kSecsPerDay, &dt);
    month = dt.month;
    day   = dt.day;
    year  = dt.year;
    if (SelectDay(selectDayByDay, &month, &day, &year, "Due date")) {
        dt.year   = year;
        dt.month  = month;
        dt.day    = day;
        dt.hour   = 0;
        dt.minute = 0;
        dt.second = 0;
        gEditDue = TimDateTimeToSeconds(&dt) / kSecsPerDay;
        if (gEditDue == autoDue)
            gEditDue = 0;               /* same as automatic */
    }
}

/* Capitalizes the first letter as it is typed; handles the due-date
 * controls without closing the dialog. */
static Boolean EditFormHandleEvent(EventPtr event)
{
    if (event->eType == ctlSelectEvent) {
        UInt16 id = event->data.ctlSelect.controlID;

        if (id == EditDue) {
            PickDue();
            SetDueLabel(FrmGetActiveForm());
            return true;
        }
        if (id == EditDueAuto) {
            gEditDue = 0;
            SetDueLabel(FrmGetActiveForm());
            return true;
        }
    }
    if (event->eType == keyDownEvent) {
        WChar chr = event->data.keyDown.chr;

        if (chr >= 'a' && chr <= 'z' &&
            !(event->data.keyDown.modifiers & commandKeyMask)) {
            FormPtr  frm = FrmGetActiveForm();
            FieldPtr fld = (FieldPtr)FrmGetObjectPtr(frm,
                                FrmGetObjectIndex(frm, EditField));

            if (FldGetInsPtPosition(fld) == 0)
                event->data.keyDown.chr = (WChar)(chr - ('a' - 'A'));
        }
    }
    return false;               /* let the field insert the character */
}

/* index == kNewItem adds a new item to the current list; otherwise edits
 * that record.  The Beam button saves the text, then beams the item. */
static void EditItem(UInt16 index)
{
    FormPtr   frm;
    FieldPtr  fld;
    MemHandle textH;
    MemPtr    p;
    Char      buf[kMaxText + 1];
    Char      out[kMaxText + 1];
    Char     *newText;
    UInt16    button;
    UInt16    beamIdx = kNewItem;

    if (index == kNewItem && gCount >= kMaxItems) {
        FrmAlert(FullAlert);
        return;
    }

    buf[0] = 0;
    if (index != kNewItem) {
        MemHandle recH = DmQueryRecord(gDB, index);
        if (recH) {
            p = MemHandleLock(recH);
            StrNCopy(buf, (Char *)p + kHdrSize, kMaxText);
            buf[kMaxText] = 0;
            MemHandleUnlock(recH);
        }
    }

    frm = FrmInitForm(EditForm);
    fld = (FieldPtr)FrmGetObjectPtr(frm, FrmGetObjectIndex(frm, EditField));
    FrmSetTitle(frm, (Char *)kEditTitles[gKind]);
    FrmSetEventHandler(frm, EditFormHandleEvent);

    textH = MemHandleNew(kMaxText + 1);
    if (!textH) {
        FrmDeleteForm(frm);
        return;
    }
    p = MemHandleLock(textH);
    StrCopy((Char *)p, buf);
    MemHandleUnlock(textH);
    FldSetTextHandle(fld, textH);

    if (index == kNewItem) {
        FrmHideObject(frm, FrmGetObjectIndex(frm, EditDelete));
        FrmHideObject(frm, FrmGetObjectIndex(frm, EditBeam));
    }

    /* Custom due date: weekly and monthly items only. */
    gEditDue = (index != kNewItem && gKind != kKindDaily) ? ReadDue(gDB, index) : 0;
    if (gKind == kKindDaily) {
        FrmHideObject(frm, FrmGetObjectIndex(frm, EditDue));
        FrmHideObject(frm, FrmGetObjectIndex(frm, EditDueAuto));
    } else {
        SetDueLabel(frm);
    }

    FrmSetFocus(frm, FrmGetObjectIndex(frm, EditField));
    FldSetInsPtPosition(fld, StrLen(buf));

    button = FrmDoDialog(frm);

    if (button == EditOK || button == EditBeam) {
        newText = FldGetTextPtr(fld);
        if (newText && newText[0]) {
            StrNCopy(out, newText, kMaxText);
            out[kMaxText] = 0;
            CapitalizeFirst(out);
            if (index == kNewItem)
                AddRecord(gDB, gKind, out, gEditDue);
            else {
                SetItemText(index, out, gEditDue);
                if (button == EditBeam)
                    beamIdx = index;
            }
        }
    } else if (button == EditDelete && index != kNewItem) {
        if (FrmAlert(DeleteAlert) == 0)
            DmRemoveRecord(gDB, index);
    }

    /* Detach and free the text handle ourselves, then drop the form. */
    FldSetTextHandle(fld, NULL);
    MemHandleFree(textH);
    FrmDeleteForm(frm);

    if (beamIdx != kNewItem)
        BeamItems(gKind, &beamIdx, 1);
}

static void ShowAbout(void)
{
    FormPtr frm = FrmInitForm(AboutForm);

    FrmDoDialog(frm);
    FrmDeleteForm(frm);
}

/* "Week starts on" dialog.  Returns true if the setting changed. */
static Boolean ChooseWeekStart(void)
{
    FormPtr frm = FrmInitForm(WeekForm);
    UInt16  button, sel;
    Boolean changed = false;

    CtlSetValue((ControlPtr)FrmGetObjectPtr(frm,
                FrmGetObjectIndex(frm, WeekDay0 + gWeekStart)), 1);
    button = FrmDoDialog(frm);
    if (button == WeekOK) {
        sel = FrmGetControlGroupSelection(frm, WeekGroup);
        if (sel != frmNoSelectedControl) {
            UInt16 day = FrmGetObjectId(frm, sel) - WeekDay0;

            if (day < 7 && day != gWeekStart) {
                gWeekStart = (UInt8)day;
                changed = true;
            }
        }
    }
    FrmDeleteForm(frm);
    return changed;
}

static void AddNewItem(void)
{
    UInt16 before = DmNumRecords(gDB);

    EditItem(kNewItem);
    if (DmNumRecords(gDB) > before)
        gTopIndex = 0xFFFF;     /* jump to the bottom so the new item shows */
    LoadTable();
}

/* ------------------------------------------------------------------ */
/* Event handling                                                      */
/* ------------------------------------------------------------------ */

static Boolean MainFormHandleEvent(EventPtr event)
{
    Boolean handled = false;

    switch (event->eType) {

    case frmOpenEvent:
        SyncTabs(FrmGetActiveForm());
        FrmDrawForm(FrmGetActiveForm());
        LoadTable();
        handled = true;
        break;

    case frmUpdateEvent:                /* after a beam arrives, etc. */
        SyncTabs(FrmGetActiveForm());
        FrmDrawForm(FrmGetActiveForm());
        LoadTable();
        handled = true;
        break;

    case ctlSelectEvent: {
        UInt16 id = event->data.ctlSelect.controlID;

        if (id == MainNewButton) {
            AddNewItem();
            handled = true;
        } else if (id >= MainTabDaily && id < MainTabDaily + kNumKinds) {
            gKind = (UInt8)(id - MainTabDaily);
            gTopIndex = 0;
            SavePrefs();
            LoadTable();
            handled = true;
        }
        break;
    }

    case menuEvent:
        switch (event->data.menu.itemID) {
        case MenuNew:
            AddNewItem();
            break;
        case MenuUncheckAll:
            UncheckAll();
            LoadTable();
            break;
        case MenuBeamList:
            BeamItems(gKind, gIdx, gCount);
            break;
        case MenuWeekStart:
            if (ChooseWeekStart()) {
                SavePrefs();
                ApplyReset();
            }
            LoadTable();
            break;
        case MenuAbout:
            ShowAbout();
            break;
        }
        handled = true;
        break;

    case tblSelectEvent: {
        TablePtr tbl   = event->data.tblSelect.pTable;
        Int16    row   = event->data.tblSelect.row;
        Int16    col   = event->data.tblSelect.column;
        UInt16   index = TblGetRowID(tbl, row);

        if (col == 0) {
            /* The table has already toggled the checkbox; store its value. */
            SetDone(index, TblGetItemInt(tbl, row, 0) != 0);
            TblMarkRowInvalid(tbl, row);
            TblRedrawTable(tbl);
            DrawCount();
        } else {
            TblUnhighlightSelection(tbl);
            EditItem(index);
            LoadTable();
        }
        handled = true;
        break;
    }

    case sclRepeatEvent:
        gTopIndex = event->data.sclRepeat.newValue;
        LoadTable();
        break;                  /* not "handled": let the scroll bar keep repeating */

    case keyDownEvent:
        if (event->data.keyDown.chr == pageUpChr) {
            ScrollPage(-1);
            handled = true;
        } else if (event->data.keyDown.chr == pageDownChr) {
            ScrollPage(1);
            handled = true;
        }
        break;

    default:
        break;
    }
    return handled;
}

static Boolean ApplicationHandleEvent(EventPtr event)
{
    if (event->eType == frmLoadEvent) {
        UInt16  formID = event->data.frmLoad.formID;
        FormPtr frm    = FrmInitForm(formID);

        FrmSetActiveForm(frm);
        if (formID == MainForm)
            FrmSetEventHandler(frm, MainFormHandleEvent);
        return true;
    }
    return false;
}

/* If the app is left open past midnight, reset and refresh. */
static void CheckNewDay(void)
{
    FormPtr frm;

    if (TodayDays() == gToday)
        return;
    ApplyReset();
    frm = FrmGetActiveForm();
    if (frm && FrmGetFormId(frm) == MainForm)
        LoadTable();
}

static void EventLoop(void)
{
    EventType event;
    Err       error;

    do {
        EvtGetEvent(&event, 60L * SysTicksPerSecond());

        if (event.eType == nilEvent) {
            CheckNewDay();
            continue;
        }
        if (SysHandleEvent(&event))
            continue;
        if (MenuHandleEvent(0, &event, &error))
            continue;
        if (ApplicationHandleEvent(&event))
            continue;
        FrmDispatchEvent(&event);
    } while (event.eType != appStopEvent);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

UInt32 PilotMain(UInt16 cmd, void *cmdPBP, UInt16 launchFlags)
{
    Err err;

    switch (cmd) {

    case sysAppLaunchCmdNormalLaunch:
        err = RomVersionCompatible(kMinROMVersion, launchFlags);
        if (err)
            return err;

        err = OpenDatabase();
        if (err) {
            FrmAlert(DatabaseErrorAlert);
            return err;
        }

        LoadPrefs();
        RegisterBeaming();
        FtrSet(kCreator, 0, 1);         /* "app is running" marker for beaming */
        ApplyReset();
        FrmGotoForm(MainForm);
        EventLoop();
        FtrUnregister(kCreator, 0);
        FrmCloseAllForms();
        SavePrefs();
        DmCloseDatabase(gDB);
        break;

    case sysAppLaunchCmdSyncNotify:
    case sysAppLaunchCmdSystemReset:
        RegisterBeaming();
        break;

    case sysAppLaunchCmdExgReceiveData: {
        UInt8   kind;
        UInt32  running = 0;
        Boolean live;

        live = (launchFlags & sysAppLaunchFlagSubCall) &&
               FtrGet(kCreator, 0, &running) == errNone && running;
        err = ReceiveBeam((ExgSocketPtr)cmdPBP, live ? gDB : 0, &kind);
        if (live && err == errNone && kind < kNumKinds) {
            FormPtr frm = FrmGetActiveForm();

            gKind = kind;
            gTopIndex = 0;
            if (frm && FrmGetFormId(frm) == MainForm)
                FrmUpdateForm(MainForm, frmRedrawUpdateCode);
        }
        break;
    }

    default:
        break;
    }
    return errNone;
}
