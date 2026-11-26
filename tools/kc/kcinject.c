// SPDX-License-Identifier: BSD-3-Clause
/*
 * kcinject: link kexts into a legacy (pre-macOS 11) x86_64 prelinked kernel with OpenCorePkg's OcAppleKernelLib, the
 * linker OpenCore uses to inject kexts at boot. Nothing else is changed: no kernel or kext patches.
 *
 *   kcinject [-v] IN OUT  BUNDLE_PATH EXEC_RELPATH EXECUTABLE INFO_PLIST  [BUNDLE_PATH ...]
 *
 * One group of four arguments per kext, in load order (dependencies first). BUNDLE_PATH is the path XNU reports
 * (e.g. /System/Library/Extensions/pthread.kext); EXEC_RELPATH is the executable's path inside the bundle
 * (Contents/MacOS/pthread); EXECUTABLE is a thin x86_64 MH_KEXT_BUNDLE file. EXEC_RELPATH and EXECUTABLE are "-"
 * for a codeless kext. IN comes from tools/kc/skeleton.py.
 *
 * Derived from OpenCorePkg Utilities/TestKextInject/KextInject.c,
 * Copyright (C) 2018, vit9696. All rights reserved. Licensed under the BSD License (BSD-3-Clause).
 */
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKernelLib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <UserFile.h>

typedef struct {
  CONST CHAR8  *BundlePath;
  CONST CHAR8  *ExecutablePath;
  UINT8        *Executable;
  UINT32       ExecutableSize;
  CHAR8        *InfoPlist;
  UINT32       InfoPlistSize;
} KEXT_ARG;

STATIC EFI_FILE_PROTOCOL  mNilFile;
STATIC UINT8              *mInput;
STATIC UINT32             mInputSize;

//
// ReadAppleKernel() reads the kernel through these (OcFileLib in firmware).
//
EFI_STATUS
OcGetFileData (
  IN  EFI_FILE_PROTOCOL  *File,
  IN  UINT32             Position,
  IN  UINT32             Size,
  OUT UINT8              *Buffer
  )
{
  if ((File != &mNilFile) || ((UINT64)Position + Size > mInputSize)) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, &mInput[Position], Size);
  return EFI_SUCCESS;
}

EFI_STATUS
OcGetFileSize (
  IN  EFI_FILE_PROTOCOL  *File,
  OUT UINT32             *Size
  )
{
  if (File != &mNilFile) {
    return EFI_INVALID_PARAMETER;
  }

  *Size = mInputSize;
  return EFI_SUCCESS;
}

STATIC
int
Usage (
  VOID
  )
{
  fprintf (stderr, "usage: kcinject [-v] IN OUT  BUNDLE_PATH EXEC_RELPATH|- EXECUTABLE|- INFO_PLIST  [...]\n");
  return 2;
}

int
ENTRY_POINT (
  int   argc,
  char  *argv[]
  )
{
  EFI_STATUS         Status;
  PRELINKED_CONTEXT  Context;
  KEXT_ARG           *Kexts;
  UINT32             KextCount;
  UINT32             Index;
  UINT32             ReservedInfoSize;
  UINT32             ReservedExeSize;
  UINT32             LinkedExpansion;
  UINT32             AllocSize;
  UINT8              *Prelinked;
  UINT32             PrelinkedSize;
  BOOLEAN            Is32Bit;
  CHAR8              BundleVersion[MAX_INFO_BUNDLE_VERSION_KEY_SIZE];
  int                Failed;

  if ((argc > 1) && (strcmp (argv[1], "-v") == 0)) {
    PcdGet32 (PcdFixedDebugPrintErrorLevel) |= DEBUG_INFO | DEBUG_VERBOSE;
    PcdGet32 (PcdDebugPrintErrorLevel)      |= DEBUG_INFO | DEBUG_VERBOSE;
    ++argv;
    --argc;
  }

  if ((argc < 3) || ((argc - 3) % 4 != 0)) {
    return Usage ();
  }

  mInput = UserReadFile (argv[1], &mInputSize);
  if (mInput == NULL) {
    fprintf (stderr, "kcinject: cannot read %s\n", argv[1]);
    return 1;
  }

  KextCount = (UINT32)(argc - 3) / 4;
  Kexts     = calloc (KextCount ? KextCount : 1, sizeof (*Kexts));
  if (Kexts == NULL) {
    return 1;
  }

  //
  // Read every kext and reserve room for it.
  //
  ReservedInfoSize = PRELINK_INFO_RESERVE_SIZE;
  ReservedExeSize  = 0;
  for (Index = 0; Index < KextCount; ++Index) {
    char  **A = &argv[3 + 4 * Index];

    Kexts[Index].BundlePath = A[0];
    if (strcmp (A[1], "-") != 0) {
      Kexts[Index].ExecutablePath = A[1];
      Kexts[Index].Executable     = UserReadFile (A[2], &Kexts[Index].ExecutableSize);
      if (Kexts[Index].Executable == NULL) {
        fprintf (stderr, "kcinject: cannot read %s\n", A[2]);
        return 1;
      }
    }

    Kexts[Index].InfoPlist = (CHAR8 *)UserReadFile (A[3], &Kexts[Index].InfoPlistSize);
    if (Kexts[Index].InfoPlist == NULL) {
      fprintf (stderr, "kcinject: cannot read %s\n", A[3]);
      return 1;
    }

    Status = PrelinkedReserveKextSize (
               &ReservedInfoSize,
               &ReservedExeSize,
               Kexts[Index].InfoPlistSize,
               Kexts[Index].Executable,
               Kexts[Index].ExecutableSize,
               FALSE
               );
    if (EFI_ERROR (Status)) {
      fprintf (stderr, "kcinject: %s does not fit (%s)\n", A[0], Status == EFI_INVALID_PARAMETER ? "bad Mach-O" : "too large");
      return 1;
    }
  }

  LinkedExpansion = KcGetSegmentFixupChainsSize (ReservedExeSize);
  if (LinkedExpansion == 0) {
    fprintf (stderr, "kcinject: kexts too large\n");
    return 1;
  }

  //
  // Load the kernel with room for the kexts (the same call OpenCore makes).
  //
  Status = ReadAppleKernel (
             &mNilFile,
             FALSE,
             &Is32Bit,
             &Prelinked,
             &PrelinkedSize,
             &AllocSize,
             ReservedInfoSize + ReservedExeSize + LinkedExpansion,
             NULL
             );
  if (EFI_ERROR (Status) || Is32Bit) {
    fprintf (stderr, "kcinject: %s is not an x86_64 kernel (%llx)\n", argv[1], (unsigned long long)Status);
    return 1;
  }

  Status = PrelinkedContextInit (&Context, Prelinked, PrelinkedSize, AllocSize, FALSE);
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "kcinject: %s is not a prelinked kernel (%llx); make it with tools/kc/skeleton.py\n", argv[1], (unsigned long long)Status);
    return 1;
  }

  printf ("kcinject: %s kernel, kexts at %llx\n", Context.IsKernelCollection ? "kernel collection" : "prelinked", (unsigned long long)Context.PrelinkedLastLoadAddress);

  Status = PrelinkedInjectPrepare (&Context, LinkedExpansion, ReservedExeSize);
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "kcinject: cannot prepare injection (%llx)\n", (unsigned long long)Status);
    return 1;
  }

  Failed = 0;
  for (Index = 0; Index < KextCount; ++Index) {
    UINT64  Address = Context.PrelinkedLastLoadAddress;

    AsciiStrCpyS (BundleVersion, sizeof (BundleVersion), "?");
    Status = PrelinkedInjectKext (
               &Context,
               NULL,
               Kexts[Index].BundlePath,
               Kexts[Index].InfoPlist,
               Kexts[Index].InfoPlistSize,
               Kexts[Index].ExecutablePath,
               Kexts[Index].Executable,
               Kexts[Index].ExecutableSize,
               BundleVersion
               );
    if (EFI_ERROR (Status)) {
      fprintf (stderr, "kcinject: FAIL %s (%llx)\n", Kexts[Index].BundlePath, (unsigned long long)Status);
      Failed = 1;
      continue;
    }

    printf (
      "kcinject: OK   %s v%s at %llx, %llu bytes\n",
      Kexts[Index].BundlePath,
      BundleVersion,
      (unsigned long long)Address,
      (unsigned long long)(Context.PrelinkedLastLoadAddress - Address)
      );
  }

  Status = PrelinkedInjectComplete (&Context);
  if (EFI_ERROR (Status)) {
    fprintf (stderr, "kcinject: cannot complete injection (%llx)\n", (unsigned long long)Status);
    return 1;
  }

  if (Failed) {
    fprintf (stderr, "kcinject: not writing %s: some kexts failed to link\n", argv[2]);
    return 1;
  }

  UserWriteFile (argv[2], Context.Prelinked, Context.PrelinkedSize);
  printf ("kcinject: wrote %s (%u bytes)\n", argv[2], Context.PrelinkedSize);
  PrelinkedContextFree (&Context);
  return 0;
}
