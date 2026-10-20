// ==== 00088324 FUN_00088324

/* WARNING: Globals starting with '_' overlap smaller symbols at the same address */

void FUN_00088324(void)

{
  *_DAT_0008832c/*0x404141*/ = 1;
  return;
}


// ==== 000883f8 FUN_000883f8

undefined8 FUN_000883f8(int param_1,uint param_2,undefined4 param_3)

{
  undefined1 uVar1;
  undefined4 uStack_14;
  undefined4 uStack_10;
  
  uStack_10 = param_3;
  if (param_1 == 2) {
    uStack_14 = param_2 & 0xffffff;
    uVar1 = FUN_0013a1d0(*(undefined4 *)(param_2 + 4));
    FUN_0009af34(uVar1,(int)&uStack_14 + 3);
    FUN_00090050(DAT_00088430/*0x167dd0*/,uVar1,uStack_14._3_1_);
  }
  else {
    uStack_14 = param_2;
    FUN_00090050(DAT_0008842c/*0x167dae "param err, format: cmd addr data\n"*/);
  }
  return CONCAT44(uStack_14,param_1);
}


// ==== 00088330 FUN_00088330

void FUN_00088330(uint param_1,undefined4 *param_2)

{
  undefined4 uVar1;
  int iVar2;
  int iVar3;
  uint uVar4;
  uint uVar5;
  uint local_24;
  uint local_20;
  int local_1c [2];
  
  iVar2 = thunk_FUN_0007f544(*param_2,UNK_000883d8);
  if (iVar2 == 0) {
    FUN_00090050(UNK_000883dc);
    if ((param_1 < 3) || ((param_1 - 1 & 1) != 0)) {
      FUN_00090050(DAT_000883f4/*0x167d65*/);
    }
    else {
      iVar2 = 1;
      param_2 = param_2 + 1;
      do {
        uVar1 = UNK_000883e0;
        FUN_0013ccb4(*param_2,UNK_000883e0,&local_24);
        FUN_0013ccb4(param_2[1],uVar1,&local_20);
        uVar5 = local_24 & 0xff;
        uVar4 = local_20 & 0xff;
        iVar3 = FUN_0009aea4(uVar5,uVar4);
        if (iVar3 == 0) {
          local_1c[0] = iVar3;
          iVar3 = FUN_0009af34(uVar5,local_1c);
          if (iVar3 == 0) {
            FUN_00090050(DAT_000883e4/*0x167ce4*/,uVar5,uVar4,local_1c[0]);
          }
          else {
            FUN_00090050(DAT_000883e8/*0x167d1b*/);
          }
        }
        else {
          FUN_00090050(DAT_000883ec/*0x167d2b*/,uVar5);
        }
        iVar2 = iVar2 + 2;
        param_2 = param_2 + 2;
      } while (iVar2 < (int)param_1);
      FUN_00090050(DAT_000883f0/*0x167d46 "Successfully set %d registers\n"*/,(int)(param_1 - 1) >> 1);
    }
  }
  return;
}


