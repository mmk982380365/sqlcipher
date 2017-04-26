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

#if SQLITE_WCDB_SIGNAL_RETRY

#include "os_wcdb.h"
#include "queue.h"
#include "sqlite3.h"
#include "sqliteInt.h"
#include "os.h"
#include <sys/errno.h>

extern Queue* WCDBInodeGetWaitQueue(unixInodeInfo* pInode);
extern sqlite3_condition* WCDBInodeGetCond(unixInodeInfo* pInode);
extern unsigned char WCDBInodeGetShared(unixInodeInfo* pInode);
extern int WCDBInodeGetFileLock(unixInodeInfo* pInode);

extern Queue* WCDBShmNodeGetWaitQueue(unixShmNode* pShmNode);
extern sqlite3_condition* WCDBShmNodeGetCond(unixShmNode* pShmNode);
extern unixShm* WCDBShmNodeGetShm(unixShmNode* pShmNode);
extern sqlite3_mutex* WCDBShmNodeGetMutex(unixShmNode* pShmNode);

extern unixShm* WCDBShmGetNext(unixShm* pShm);
extern u16 WCDBShmGetExclMask(unixShm* pShm);
extern u16 WCDBShmGetSharedMask(unixShm* pShm);

extern unsigned char WCDBFileGetFileLock(unixFile* pFile);
extern unixShm* WCDBFileGetShm(unixFile* pFile);

void WCDBSignal(unixInodeInfo* pInode)
{
    WCDBWaitInfo* pWaitInfo = (WCDBWaitInfo*)sqlite3QueuePop(WCDBInodeGetWaitQueue(pInode));
    if (pWaitInfo) {
        pthreadCondSignal(WCDBInodeGetCond(pInode), pWaitInfo->pThread);
        pthreadFree(pWaitInfo->pThread);
        sqlite3_free(pWaitInfo);
    }
}

void WCDBTrySignal(unixInodeInfo* pInode)
{
    int rc;
    do {
        rc = SQLITE_BUSY;
        WCDBWaitInfo* pWaitInfo = (WCDBWaitInfo*)sqlite3QueueFront(WCDBInodeGetWaitQueue(pInode));
        if (!pWaitInfo) {
            break;
        }
        int eFlag = pWaitInfo->eFlag;
        int eFileLock = pWaitInfo->eFileLock;
        unixFile* pFile = pWaitInfo->pFile;
        switch (eFlag) {
            case SQLITE_WAIT_SHARED: {
                rc = SQLITE_OK;
                if( (WCDBFileGetFileLock(pFile)!=WCDBInodeGetFileLock(pInode) &&
                     (WCDBInodeGetFileLock(pInode)>=PENDING_LOCK || eFileLock>SHARED_LOCK)) ) {
                    rc = SQLITE_BUSY;
                }
                break;
            }
            case SQLITE_WAIT_EXCLUSIVE: {
                rc = SQLITE_OK;
                if( eFileLock==EXCLUSIVE_LOCK && WCDBInodeGetShared(pInode)>1 ) {
                    rc = SQLITE_BUSY;
                }
                break;
            }
            default:
                break;
        }
        if (rc==SQLITE_OK) {
            WCDBSignal(pInode);
        }
    } while (rc==SQLITE_OK);
}

void WCDBWait(unixInodeInfo* pInode, unixFile* pFile, int eFileLock, int eFlag)
{
    WCDBWaitInfo* lastInfo = (WCDBWaitInfo*)sqlite3QueueFront(WCDBInodeGetWaitQueue(pInode));
    if (lastInfo) {
        if ( (eFileLock==EXCLUSIVE_LOCK&&lastInfo->eFileLock>SHARED_LOCK)
            || (lastInfo->eFileLock==EXCLUSIVE_LOCK&&eFileLock>SHARED_LOCK)) {
            WCDBSignal(pInode);
            return;
        }
    }
    WCDBWaitInfo* info = (WCDBWaitInfo*)sqlite3_malloc(sizeof(WCDBWaitInfo));
    info->eFlag = eFlag;
    info->eFileLock = eFileLock;
    info->pFile = pFile;
    info->pThread = pthreadAlloc();
    pthreadSelf(info->pThread);
    if (pthreadIsMain()) {
        sqlite3QueuePushFront(WCDBInodeGetWaitQueue(pInode), info);
    }else {
        sqlite3QueuePush(WCDBInodeGetWaitQueue(pInode), info);
    }
    if (pthreadCondWait(WCDBInodeGetCond(pInode), unixVFSMutex(), 10)==ETIMEDOUT) {
        sqlite3_log(SQLITE_WARNING, "Wait Failed With Timeout");
    }
}

void WCDBShmSignal(unixShmNode* pShmNode)
{
  WCDBShmWaitInfo* pInfo = (WCDBShmWaitInfo*)sqlite3QueuePop(WCDBShmNodeGetWaitQueue(pShmNode));
  if (pInfo) {
    pthreadCondSignal(WCDBShmNodeGetCond(pShmNode), pInfo->pThread);
    pthreadFree(pInfo->pThread);
    sqlite3_free(pInfo);
  }
}

void WCDBShmTrySignal(unixShmNode* pShmNode){
  int rc;
  do {
    rc = SQLITE_BUSY;
    WCDBShmWaitInfo* pInfo = (WCDBShmWaitInfo*)sqlite3QueueFront(WCDBShmNodeGetWaitQueue(pShmNode));
    if (!pInfo) {
      break;
    }
    int eFlag = pInfo->eFlag;
    int oMask = pInfo->oMask;
    switch (eFlag) {
      case SQLITE_SHM_SHARED: {
        unixShm* pX;
        rc = SQLITE_OK;
        for(pX=WCDBShmNodeGetShm(pShmNode); pX; pX=WCDBShmGetNext(pX)){
          if( (WCDBShmGetExclMask(pX) & oMask)!=0 ){
            rc = SQLITE_BUSY;
            break;
          }
        }
        break;
      }
      case SQLITE_SHM_EXCLUSIVE: {
        unixShm* pX;
        rc = SQLITE_OK;
        for(pX=WCDBShmNodeGetShm(pShmNode); pX; pX=WCDBShmGetNext(pX)){
          if( (WCDBShmGetExclMask(pX) & oMask)!=0 || (WCDBShmGetSharedMask(pX) & oMask)!=0 ){
            rc = SQLITE_BUSY;
            break;
          }
        }
        break;
      }
      default:
        break;
    }
    if (rc==SQLITE_OK) {
      WCDBShmSignal(pShmNode);
    }
  } while (rc==SQLITE_OK);
}

void WCDBShmWait(unixShmNode* pShmNode, unixFile* pFile, int oMask, int eFlag)
{
  WCDBShmWaitInfo* lastInfo = (WCDBShmWaitInfo*)sqlite3QueueFront(WCDBShmNodeGetWaitQueue(pShmNode));
  if (lastInfo) {
    if (lastInfo->eFlag==SQLITE_SHM_EXCLUSIVE
        &&((WCDBShmGetExclMask(WCDBFileGetShm(pFile))&lastInfo->oMask)!=0
           ||(WCDBShmGetSharedMask(WCDBFileGetShm(pFile))&lastInfo->oMask)!=0)) {
      WCDBShmSignal(pShmNode);
      return;
    }
    if (eFlag==SQLITE_SHM_EXCLUSIVE
        &&((WCDBShmGetExclMask(WCDBFileGetShm(lastInfo->pFile))&oMask)!=0
           ||(WCDBShmGetSharedMask(WCDBFileGetShm(lastInfo->pFile))&oMask)!=0)) {
      WCDBShmSignal(pShmNode);
      return;
    }
  }
  
  WCDBShmWaitInfo* info = (WCDBShmWaitInfo*)sqlite3_malloc(sizeof(WCDBShmWaitInfo));
  info->eFlag = eFlag;
  info->oMask = oMask;
  info->pFile = pFile;
  info->pThread = pthreadAlloc();
  pthreadSelf(info->pThread);
  if (pthreadIsMain()) {
    sqlite3QueuePushFront(WCDBShmNodeGetWaitQueue(pShmNode), info);
  }else {
    sqlite3QueuePush(WCDBShmNodeGetWaitQueue(pShmNode), info);
  }
  if (pthreadCondWait(WCDBShmNodeGetCond(pShmNode), WCDBShmNodeGetMutex(pShmNode), 10)==ETIMEDOUT) {
    sqlite3_log(SQLITE_WARNING, "Wait Failed With Timeout");
  }
}

#endif// SQLITE_WCDB_SIGNAL_RETRY
