/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef os_wcdb_h
#define os_wcdb_h

#if SQLITE_WCDB_SIGNAL_RETRY

#include "mutex_wcdb.h"

#define SQLITE_WAIT_NONE 0
#define SQLITE_WAIT_EXCLUSIVE 1
#define SQLITE_WAIT_SHARED 2

typedef struct unixInodeInfo unixInodeInfo;
typedef struct unixFile unixFile;

typedef struct unixShmNode unixShmNode;
typedef struct unixShm unixShm;

typedef struct WCDBWaitInfo WCDBWaitInfo;
struct WCDBWaitInfo {
  sqlite3_thread* pThread;
  int eFileLock;
  int eFlag;
  unixFile* pFile;
};

typedef struct WCDBShmWaitInfo WCDBShmWaitInfo;
struct WCDBShmWaitInfo {
  sqlite3_thread* pThread;
  int oMask;
  int eFlag;
  unixFile* pFile;
};

void WCDBSignal(unixInodeInfo* pInode);
void WCDBTrySignal(unixInodeInfo* pInode);
void WCDBWait(unixInodeInfo* pInode, unixFile* pFile, int eFileLock, int eFlag);

void WCDBShmSignal(unixShmNode* pShmNode);
void WCDBShmTrySignal(unixShmNode* pShmNode);
void WCDBShmWait(unixShmNode* pShmNode, unixFile* pFile, int oMask, int eFlag);

#endif// SQLITE_WCDB_SIGNAL_RETRY

#endif /* os_wcdb_h */
