/* tasklists_res.h - resource IDs shared by tasklists.c and tasklists.rcp */

#define MainForm            1000
#define MainTable           1001
#define MainScrollBar       1002
#define MainNewButton       1003
#define MainTabDaily        1004    /* tab IDs must stay consecutive: */
#define MainTabWeekly       1005    /* ID = MainTabDaily + list kind  */
#define MainTabMonthly      1006
#define MainTabGroup        1

#define EditForm            1100
#define EditField           1101
#define EditOK              1102
#define EditCancel          1103
#define EditDelete          1104
#define EditBeam            1105
#define EditDue             1106    /* selector trigger: pick a due date */
#define EditDueAuto         1107    /* back to the automatic due date */

#define MainMenu            1200
#define MenuNew             1201
#define MenuUncheckAll      1202
#define MenuAbout           1203
#define MenuBeamList        1204
#define MenuWeekStart       1205

#define DeleteAlert         1300
#define RomIncompatibleAlert 1302
#define DatabaseErrorAlert  1303
#define FullAlert           1304
#define BeamFailAlert       1305
#define NoBeamAlert         1306

#define AboutForm           1400
#define AboutOK             1401
#define JakesoftLogoBitmap  1402

#define WeekForm            1500
#define WeekDay0            1501    /* 1501..1507 = Sun..Sat, consecutive */
#define WeekDay1            1502
#define WeekDay2            1503
#define WeekDay3            1504
#define WeekDay4            1505
#define WeekDay5            1506
#define WeekDay6            1507
#define WeekOK              1510
#define WeekCancel          1511
#define WeekGroup           2
